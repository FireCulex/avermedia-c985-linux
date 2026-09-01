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

    struct mutex lock;
    wait_queue_head_t wq;
    struct kfifo fifo;
    bool running;

    void *dma_buf;          /* DMA-coherent bounce buffer for frame reads */
    dma_addr_t dma_phys;
    size_t dma_size;

    atomic64_t frames_done;
    atomic64_t bytes_done;
    atomic64_t frames_dropped;   /* fifo full or no data */
};

/* ---- audio_consumer: called from mbox drain work for taskId==1 descs ---- */

static void c985_audio_consume(struct c985_dev *dev, struct c985_frame_desc *d)
{
    struct c985_audio *a = dev->audio_priv;
    int n;

    if (!a || !a->running) {
        dev_dbg(&dev->pdev->dev, "audio: desc dropped, not running\n");
        return;
    }

    /* Class-4 (compressed audio) descriptor: addr = y_dw (DWORDS -> <<2 for
     * bytes), size = chroma field in DWORDS (firmware reserved3), so -> <<2
     * for byte length. Verified against the Windows class-4 record build
     * (addrDWORDS=0x6B8, sizeDWORDS=0x6C0). */
    u32 bytes = d->chroma << 2;

    if (d->y_dw == 0 || d->chroma == 0 || bytes > a->dma_size) {
        dev_dbg(&dev->pdev->dev, "audio: bad desc addr=0x%x size_dw=%u\n",
                d->y_dw, d->chroma);
        atomic64_inc(&a->frames_dropped);
        return;
    }

    /* Linear read (no frame geometry): card buffer addr << 2, size = bytes. */
    n = c985_dma_read_linear(dev, d->y_dw << 2, a->dma_phys, bytes);
    if (n < 0) {
        dev_dbg(&dev->pdev->dev, "audio: DMA read failed: %d\n", n);
        atomic64_inc(&a->frames_dropped);
        return;
    }

    mutex_lock(&a->lock);
    if (kfifo_in(&a->fifo, a->dma_buf, bytes) < bytes)
        atomic64_inc(&a->frames_dropped);
    mutex_unlock(&a->lock);

    atomic64_inc(&a->frames_done);
    atomic64_add(bytes, &a->bytes_done);
    wake_up_all(&a->wq);
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
    substream->runtime->hw = c985_audio_hw;
    substream->private_data = a;
    return 0;
}

static int c985_audio_pcm_close(struct snd_pcm_substream *substream)
{
    (void)substream;
    return 0;
}

static int c985_audio_pcm_hw_params(struct snd_pcm_substream *substream,
                                    struct snd_pcm_hw_params *params)
{
    return snd_pcm_lib_malloc_pages(substream,
                                    params_buffer_bytes(params));
}

static int c985_audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    return snd_pcm_lib_free_pages(substream);
}

static int c985_audio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct c985_audio *a = substream->pcm->private_data;
    mutex_lock(&a->lock);
    kfifo_reset(&a->fifo);
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
    return bytes_to_frames(substream->runtime,
                           (unsigned long)atomic64_read(&a->bytes_done));
}

static int c985_audio_pcm_copy(struct snd_pcm_substream *substream, int channel,
                               unsigned long pos, struct iov_iter *iter,
                               unsigned long count)
{
    struct c985_audio *a = substream->pcm->private_data;
    u8 tmp[512];
    size_t copied = 0;
    int ret;

    /* count is bytes for the compressed (MPEG) format; zero means done. */
    if (count == 0)
        return 0;

    ret = wait_event_interruptible(a->wq,
                                   kfifo_len(&a->fifo) > 0 || !a->running);
    if (ret)
        return ret;
    if (!a->running && kfifo_len(&a->fifo) == 0)
        return -ENODATA;

    while (copied < count) {
        unsigned int chunk = min_t(unsigned int, sizeof(tmp), count - copied);
        unsigned int got;

        mutex_lock(&a->lock);
        got = kfifo_out(&a->fifo, tmp, chunk);
        mutex_unlock(&a->lock);
        if (got == 0)
            break;

        if (copy_to_iter(tmp, got, iter) != got) {
            ret = -EFAULT;
            return ret;
        }
        copied += got;
    }

    return copied;
}

static const struct snd_pcm_ops c985_audio_pcm_ops = {
    .open      = c985_audio_pcm_open,
    .close     = c985_audio_pcm_close,
    .hw_params = c985_audio_pcm_hw_params,
    .hw_free   = c985_audio_pcm_hw_free,
    .prepare   = c985_audio_pcm_prepare,
    .trigger   = c985_audio_pcm_trigger,
    .pointer   = c985_audio_pcm_pointer,
    .copy      = c985_audio_pcm_copy,
};

/* ---- audio task boot (taskId 1) ---- */

int c985_audio_boot(struct c985_dev *dev)
{
    struct c985_audio *a = dev->audio_priv;
    static const struct { u16 opcode; u32 param0; u32 slots[3]; int nslots; } steps[] = {
        { 0xF1, C985_FUNC_AUDIO, { 0 }, 0 },
        { 0xF2, 0x00010000,     { 0 }, 0 },
        { 0x10, 0x0F,           { 0, 0 }, 2 },
        { 0x10, 0x10,           { 0, 0, 0 }, 3 },
        { 0x10, 0x02,           { 0xf1f1f1da, 0xb6f1f1b6 }, 2 },
    };
    /* Audio encoder descriptor block (0x6F8..0x6D0), live-trace verified. */
    u32 cfg[C985_ENC_CFG_REGS] = {
        0x2101b214, 0x04380780, 0x0f7c0609, 0x005003e8, 0x1f4007d0,
        0x80002000, 0x6001000a, 0x10, 0x04380780, 0x01121080, 0x480,
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

    ret = c985_mbox_send_polling(dev, 0x06, C985_AUD_TASK, C985_AUD_TASK,
                                 false, true, 300);
    if (ret && ret != -ETIMEDOUT)
        return ret;

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

    if (a->card)
        snd_card_free(a->card);

    if (a->dma_buf)
        dma_free_coherent(&dev->pdev->dev, a->dma_size, a->dma_buf,
                          a->dma_phys);
    kfifo_free(&a->fifo);
    dev->audio_priv = NULL;
    devm_kfree(&dev->pdev->dev, a);
}