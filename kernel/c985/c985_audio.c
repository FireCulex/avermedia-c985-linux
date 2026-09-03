// SPDX-License-Identifier: GPL-2.0
/*
 * c985_audio.c - ALSA audio capture for the AVerMedia C985.
 *
 * The card's native record format is MPEG4 (H.264 + AAC): the audio task
 * (taskId 1, function 0x80000040) emits AAC-LC frames (@48kHz/128kbps/
 * stereo in the reference OBS session). Those frames are delivered on the
 * same 0x40/0x41 EncDataOutReq mailbox descriptor as video, tagged with
 * taskId=1 in the upper 16 bits of 0x6B0, and are DMA-read LINEARLY
 * (ctrl 0x0804200F, no frame geometry) in 1536..4608 byte chunks.
 *
 * We expose the compressed stream through an ALSA PCM capture device using
 * SNDRV_PCM_FORMAT_MPEG (the standard compressed-passthrough format, same
 * mechanism as HDMI AC3/DTS/AAC drivers). A kfifo buffers DMA'd frames; the
 * copy_user path drains it. No sample parsing is done (the data is opaque
 * AAC; userspace treats it as a byte stream).
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kfifo.h>
#include <linux/uio.h>
#include <linux/dma-mapping.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "c985.h"

#define C985_AUDIO_NAME       "c985-audio"
/* AAC frames are <=4608 bytes; keep a generous software FIFO. */
#define C985_AUDIO_FIFO_BYTES (256 * 1024)

struct c985_audio {
    struct c985_dev *dev;

    struct snd_card *card;
    struct snd_pcm *pcm;
    struct snd_pcm_substream *substream;

    struct mutex lock;
    wait_queue_head_t wq;
    struct kfifo fifo;
    bool running;

    void *dma_buf;          /* DMA-coherent bounce buffer for frame reads */
    dma_addr_t dma_phys;
    size_t dma_size;

    struct c985_frame_op op; /* single in-flight async linear audio DMA op */

    atomic64_t frames_done;
    atomic64_t bytes_done;
    atomic64_t frames_dropped;   /* fifo full or no data */

    unsigned long hw_ptr;        /* consumed buffer position (bytes), wraps */
    unsigned long last_period;   /* last signaled period index */
};

/* ---- audio_consumer + async completion (taskId==1 descs) ---- */

/* DMA completion: run in dma_frame_wq (process context) after the linear
 * read lands. Copy the bounce buffer into the ALSA ring/fifo, release the
 * firmware ring slot (0x30), and re-pump the mbox drain work. */
static void c985_audio_done(struct c985_frame_op *op)
{
    struct c985_dev *dev = op->dev;
    struct c985_audio *a = dev->audio_priv;
    struct snd_pcm_substream *substream;
    struct snd_pcm_runtime *runtime;
    unsigned long buf_bytes, period_bytes;
    u8 *dma_area;
    unsigned long off;
    u32 bytes = op->desc.chroma << 2;

    /* Copy bounce -> ALSA ring (or kfifo if no substream yet). */
    substream = READ_ONCE(a->substream);
    if (!substream || !substream->runtime)
        goto fifo;

    runtime = substream->runtime;
    dma_area = runtime->dma_area;
    buf_bytes = snd_pcm_lib_buffer_bytes(substream);
    if (!dma_area || buf_bytes == 0)
        goto fifo;

    /* Write the frame into the ring at hw_ptr, wrapping; then advance and
     * signal the ALSA core so its mmap/copy_user read completes. */
    off = a->hw_ptr % buf_bytes;
    if (off + bytes > buf_bytes) {
        unsigned long first = buf_bytes - off;
        memcpy(dma_area + off, a->dma_buf, first);
        memcpy(dma_area, a->dma_buf + first, bytes - first);
    } else {
        memcpy(dma_area + off, a->dma_buf, bytes);
    }
    a->hw_ptr += bytes;

    period_bytes = frames_to_bytes(runtime, runtime->period_size);
    if (a->hw_ptr / period_bytes > a->last_period) {
        a->last_period = a->hw_ptr / period_bytes;
        snd_pcm_period_elapsed(substream);
    }

    goto accounted;

fifo:
    mutex_lock(&a->lock);
    if (kfifo_in(&a->fifo, a->dma_buf, bytes) < bytes)
        atomic64_inc(&a->frames_dropped);
    mutex_unlock(&a->lock);

accounted:
    if (!op->failed) {
        atomic64_inc(&a->frames_done);
        atomic64_add(bytes, &a->bytes_done);
    } else {
        atomic64_inc(&a->frames_dropped);
    }
    wake_up_all(&a->wq);

    /* Release the card ring slot now that the DMA has actually completed. */
    c985_mbox_release_desc(dev, &op->desc);

    /* Engine is free: re-pump the drain work so queued descriptors flow. */
    queue_work(dev->mbox_drain_wq, &dev->mbox_drain_work.work);
}

/* audio_consumer: called from mbox drain work for taskId==1 descriptors.
 * Submits the linear DMA read ASYNCHRONOUSLY (Windows sync(0)/DPC model) and
 * returns immediately. Return: 0 = in flight, -EBUSY = engine busy (deferral),
 * <0 = dropped (ring slot already released). */
static int c985_audio_consume(struct c985_dev *dev, struct c985_frame_desc *d)
{
    struct c985_audio *a = dev->audio_priv;
    u32 bytes;
    int ret;

    if (!a || !a->running) {
        dev_dbg(&dev->pdev->dev, "audio: desc dropped, not running\n");
        c985_mbox_release_desc(dev, d);
        return 0;
    }

    /* Class-4 (compressed audio) descriptor: addr = y_dw (DWORDS -> <<2 for
     * bytes), size = chroma field in DWORDS (firmware reserved3), so -> <<2
     * for byte length. Verified against the Windows class-4 record build
     * (addrDWORDS=0x6B8, sizeDWORDS=0x6C0). */
    bytes = d->chroma << 2;

    if (d->y_dw == 0 || d->chroma == 0 || bytes > a->dma_size) {
        dev_dbg(&dev->pdev->dev, "audio: bad desc addr=0x%x size_dw=%u\n",
                d->y_dw, d->chroma);
        atomic64_inc(&a->frames_dropped);
        c985_mbox_release_desc(dev, d);
        return -EINVAL;
    }

    /* Linear read (no frame geometry): card buffer addr << 2, size = bytes. */
    a->op.dev = dev;
    a->op.desc = *d;
    a->op.linear = true;
    a->op.card_addr = d->y_dw << 2;
    a->op.len = bytes;
    a->op.dst_phys = a->dma_phys;
    a->op.done_cb = c985_audio_done;

    ret = c985_dma_submit_linear(&a->op);
    if (ret) {
        dev_dbg(&dev->pdev->dev, "audio: async DMA submit failed: %d\n", ret);
        if (ret == -EBUSY) {
            /* Engine busy: defer, do NOT release the ring slot. */
            return -EBUSY;
        }
        atomic64_inc(&a->frames_dropped);
        c985_mbox_release_desc(dev, d);
        return -EINVAL;
    }

    return 0;
}

/* ---- ALSA PCM capture callbacks ---- */

static struct snd_pcm_hardware c985_audio_hw = {
    .info = SNDRV_PCM_INFO_INTERLEAVED,
    .formats = SNDRV_PCM_FMTBIT_S16_LE,
    .rates = SNDRV_PCM_RATE_48000,
    .rate_min = 48000,
    .rate_max = 48000,
    .channels_min = 2,
    .channels_max = 2,
    .buffer_bytes_max = C985_AUDIO_FIFO_BYTES,
    .period_bytes_min = 1536,
    .period_bytes_max = 4608,
    .periods_min = 1,
    .periods_max = 1024,
};

/*
 * AAC-LC passthrough: the firmware emits compressed AAC, but ALSA cannot
 * negotiate a zero-width compressed format (MPEG is filtered out in
 * snd_pcm_hw_rule_format). We therefore expose S16_LE as an opaque BYTE
 * transport: the interleaved 2ch framing is ignored by userspace, which
 * decodes the raw AAC bitstream itself (ffmpeg -f alsa -> -f adts). The
 * kfifo carries raw AAC bytes; period/buffer math is still well-defined
 * because S16_LE has a real 16-bit width.
 */
static int c985_audio_pcm_open(struct snd_pcm_substream *substream)
{
    struct c985_audio *a = substream->pcm->private_data;
    snd_pcm_set_sync(substream);
    substream->runtime->hw = c985_audio_hw;
    a->substream = substream;
    return 0;
}

static int c985_audio_pcm_close(struct snd_pcm_substream *substream)
{
    struct c985_audio *a = substream->pcm->private_data;
    a->substream = NULL;
    return 0;
}

static int c985_audio_pcm_hw_params(struct snd_pcm_substream *substream,
                                    struct snd_pcm_hw_params *params)
{
    return 0; /* managed buffer already set via snd_pcm_set_managed_buffer_all */
}

static int c985_audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    return 0;
}

static int c985_audio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct c985_audio *a = substream->pcm->private_data;
    mutex_lock(&a->lock);
    kfifo_reset(&a->fifo);
    a->hw_ptr = 0;
    a->last_period = 0;
    mutex_unlock(&a->lock);
    return 0;
}

static int c985_audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct c985_audio *a = substream->pcm->private_data;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        a->running = true;
        return 0;
    case SNDRV_PCM_TRIGGER_STOP:
        a->running = false;
        wake_up_all(&a->wq);
        return 0;
    default:
        return -EINVAL;
    }
}

static snd_pcm_uframes_t c985_audio_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct c985_audio *a = substream->pcm->private_data;
    unsigned long buf_bytes = snd_pcm_lib_buffer_bytes(substream);

    return bytes_to_frames(substream->runtime, a->hw_ptr % buf_bytes);
}

static const struct snd_pcm_ops c985_audio_pcm_ops = {
    .open      = c985_audio_pcm_open,
    .close     = c985_audio_pcm_close,
    .hw_params = c985_audio_pcm_hw_params,
    .hw_free   = c985_audio_pcm_hw_free,
    .prepare   = c985_audio_pcm_prepare,
    .trigger   = c985_audio_pcm_trigger,
    .pointer   = c985_audio_pcm_pointer,
};

/* ---- audio task boot (taskId 1) ---- */

int c985_audio_boot(struct c985_dev *dev)
{
    struct c985_audio *a = dev->audio_priv;
    static const struct { u16 opcode; u32 param0; u32 slots[3]; int nslots; } steps[] = {
        { 0xF1, C985_FUNC_AUDIO, { 0 }, 0 },
        { 0xF2, 0x1000108,      { 0 }, 0 },
        { 0x10, 0x0F,           { 0, 0 }, 2 },
        { 0x10, 0x10,           { 0, 0, 0 }, 3 },
        { 0x10, 0x12,           { 0 }, 1 },
        { 0x10, 0x13,           { 0x50, 0, 0xA }, 3 },
        { 0x10, 0x14,           { 1, 0x4A38 }, 2 },
        { 0x10, 0x04,           { 0, 0, 0 }, 3 },  /* SetAudioEnhancement (8 slots, first 3) */
        { 0x10, 0x02,           { 0xf1f1f1da, 0xb6f1f1b6 }, 2 },
    };
    /* Audio encoder descriptor block (0x6F8..0x6D0), member order from
     * dbgview.log QPFWENCAPI_Set* burst for taskId=1 (81.853s):
     * SetSystemControl(0x6F8), SetPictureResolution(0x6F4), SetInputControl(0x6F0),
     * SetRateControl(0x6EC), SetVBRBitRate(0x6E8), SetFilterControl(0x6E4),
     * SetGOPLoopFilter(0x6E0), SetOutPicResolution(0x6DC), SetBlockSize(0x6D8),
     * SetAudioControl(0x6D4), SetAudioControlEx(0x6D0). */
u32 cfg[C985_ENC_CFG_REGS] = {
        0x2101b214, 0x04380780, 0x0f7c0609, 0x005003e8, 0x1f4007d0,
        0x80002000, 0x6001000a, 0x04380780, 0x10, 0x01121080, 0x480,
    };
    int i, ret;

    for (i = 0; i < ARRAY_SIZE(steps); i++) {
        int j;
        for (j = 0; j < ARRAY_SIZE(dev->mbox_slots); j++)
            dev->mbox_slots[j] = (j < steps[i].nslots) ? steps[i].slots[j] : 0;

        ret = c985_mbox_send_polling(dev, steps[i].opcode, steps[i].param0,
                                     C985_AUD_TASK, false, false, 300);
        if (ret && ret != -ETIMEDOUT) {
            dev_err(&dev->pdev->dev, "audio boot step 0x%02x failed: %d\n",
                    steps[i].opcode, ret);
            return ret;
        }
    }

    c985_enc_write_config(dev, cfg);

    ret = c985_mbox_send_polling(dev, 0x01, C985_AUD_TASK, C985_AUD_TASK,
                                 false, true, 300);
    if (ret && ret != -ETIMEDOUT)
        return ret;

    if (a) {
        mutex_lock(&a->lock);
        kfifo_reset(&a->fifo);
        mutex_unlock(&a->lock);
    }
    return 0;
}

void c985_audio_stop(struct c985_dev *dev)
{
    c985_mbox_send_polling(dev, 0x02, 0, C985_AUD_TASK, false, false, 500);
}

void c985_audio_teardown(struct c985_dev *dev)
{
    c985_mbox_send_polling(dev, 0xF3, 0, C985_AUD_TASK, false, false, 500);
    msleep(100);
}

/* ---- init / cleanup ---- */

void c985_audio_get_stats(struct c985_dev *dev, struct c985_stream_stats *s)
{
    struct c985_audio *a = dev->audio_priv;

    if (!a) {
        memset(s, 0, sizeof(*s));
        return;
    }
    atomic64_set(&s->frames_done, atomic64_read(&a->frames_done));
    atomic64_set(&s->frames_dropped, atomic64_read(&a->frames_dropped));
    atomic64_set(&s->bytes_done, atomic64_read(&a->bytes_done));
    atomic64_set(&s->desc_non40, 0);
    atomic64_set(&s->desc_bad, 0);
    atomic64_set(&s->no_buf, 0);
}

int c985_audio_init(struct c985_dev *dev)
{
    struct c985_audio *a;
    struct snd_card *card;
    int ret;

    a = devm_kzalloc(&dev->pdev->dev, sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    a->dev = dev;
    mutex_init(&a->lock);
    init_waitqueue_head(&a->wq);
    a->running = false;
    atomic64_set(&a->frames_done, 0);
    atomic64_set(&a->bytes_done, 0);
    atomic64_set(&a->frames_dropped, 0);

    ret = kfifo_alloc(&a->fifo, C985_AUDIO_FIFO_BYTES, GFP_KERNEL);
    if (ret)
        goto err;

    a->dma_size = C985_AUDIO_FRAME_MAX;
    a->dma_buf = dma_alloc_coherent(&dev->pdev->dev, a->dma_size,
                                    &a->dma_phys, GFP_KERNEL);
    if (!a->dma_buf) {
        dev_err(&dev->pdev->dev, "audio: DMA bounce buffer alloc failed\n");
        ret = -ENOMEM;
        goto err_fifo;
    }

    dev->audio_priv = a;
    dev->audio_consumer = c985_audio_consume;

    ret = snd_card_new(&dev->pdev->dev, -1, NULL, THIS_MODULE, 0, &card);
    if (ret < 0)
        goto err_fifo;
    a->card = card;

    strscpy(card->driver, "c985", sizeof(card->driver));
    strscpy(card->shortname, C985_AUDIO_NAME, sizeof(card->shortname));
    strscpy(card->longname, "AVerMedia C985 AAC Capture",
            sizeof(card->longname));

    ret = snd_pcm_new(card, C985_AUDIO_NAME, 0, 0, 1, &a->pcm);
    if (ret < 0)
        goto err_card;

    snd_pcm_set_ops(a->pcm, SNDRV_PCM_STREAM_CAPTURE, &c985_audio_pcm_ops);
    a->pcm->private_data = a;
    a->pcm->info_flags = 0;
    a->pcm->nonatomic = true;   /* our callbacks sleep (kfifo + DMA wait) */

    snd_pcm_set_managed_buffer_all(a->pcm,
                                   SNDRV_DMA_TYPE_DEV,
                                   &dev->pdev->dev,
                                   C985_AUDIO_FIFO_BYTES,
                                   C985_AUDIO_FIFO_BYTES);

    ret = snd_card_register(card);
    if (ret < 0)
        goto err_card;

    dev_info(&dev->pdev->dev, "audio capture registered as %s\n", card->id);
    return 0;

err_card:
    snd_card_free(card);
    a->card = NULL;
    a->pcm = NULL;
err_fifo:
    if (a->dma_buf)
        dma_free_coherent(&dev->pdev->dev, a->dma_size, a->dma_buf,
                          a->dma_phys);
    kfifo_free(&a->fifo);
    dev->audio_priv = NULL;
    dev->audio_consumer = NULL;
err:
    devm_kfree(&dev->pdev->dev, a);
    return ret;
}

void c985_audio_cleanup(struct c985_dev *dev)
{
    struct c985_audio *a = dev->audio_priv;

    if (!a)
        return;

    dev->audio_consumer = NULL;
    a->running = false;
    wake_up_all(&a->wq);

    /* An in-flight async linear DMA op holds dev->dma_cur_op pointing at
     * a->op; flush the frame workqueue so the done_cb runs (and clears the
     * slot) before we free the bounce buffer and the op's backing memory. */
    if (dev->dma_frame_wq)
        flush_workqueue(dev->dma_frame_wq);

    if (a->card)
        snd_card_free(a->card);

    if (a->dma_buf)
        dma_free_coherent(&dev->pdev->dev, a->dma_size, a->dma_buf,
                          a->dma_phys);
    kfifo_free(&a->fifo);
    dev->audio_priv = NULL;
    devm_kfree(&dev->pdev->dev, a);
}