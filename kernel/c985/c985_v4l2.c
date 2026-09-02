// SPDX-License-Identifier: GPL-2.0
/*
 * c985_v4l2.c - V4L2 capture device + videobuf2 (vb2_dma_contig) queue for
 * the AverMedia C985 PCIe capture card.
 *
 * Architecture:
 *   start_streaming boots the encoder (polling mbox: F1/F2/six 0x10/0x01)
 *   and starts a kthread (c985_v4l2_thread). The thread polls the mailbox
 *   (0x6C8 bit0) continuously, reads each 0x40 frame descriptor, does three
 *   synchronous frame-mode DMA reads (Y, U, V=U+0x40) into the next queued
 *   vb2 buffer, DQBUFs it, and releases the card ring slot via 0x30
 *   (CompleteArm).
 *
 * Buffer model: one vb2_queue, V4L2_BUF_TYPE_VIDEO_CAPTURE (single-planar)
 * with one vb2_dma_contig buffer of C985_FRAME_BYTES holding YUV420p
 * (Y, then U, then V contiguous). OBS requires V4L2_CAP_VIDEO_CAPTURE.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kthread.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-dma-contig.h>

#include "c985.h"

#define C985_V4L2_NAME    "c985-video"

struct c985_v4l2 {
    struct c985_dev *dev;

    struct video_device vdev;
    struct v4l2_device v4l2_dev;

    struct vb2_queue queue;
    struct mutex q_lock;          /* serializes queue ops */

    /* ready-list of queued-but-not-yet-captured buffers */
    struct list_head ready;
    unsigned int queued_count;

    /* streaming kthread */
    struct task_struct *thread;
    bool thread_stop;             /* set to request thread exit */

    /* stats */
    atomic64_t frames_done;
    atomic64_t frames_dropped;
    atomic64_t desc_non40;     /* 0x40 polling but msg != 0x40 */
    atomic64_t desc_bad;       /* 0x40 but zero addrs */
    atomic64_t no_buf;         /* 0x40 valid but no queued vb2 buffer */
};

/* Per-vb2-buffer driver state, embedded ahead of vb2_v4l2_buffer. */
struct c985_buf {
    struct vb2_v4l2_buffer vb;
    struct list_head list;
};

static inline struct c985_buf *to_c985_buf(struct vb2_buffer *vb2)
{
    return container_of(to_vb2_v4l2_buffer(vb2), struct c985_buf, vb);
}

/* Get the next queued buffer. Returns NULL if none. Caller holds q_lock. */
static struct c985_buf *c985_v4l2_next_buf(struct c985_v4l2 *c)
{
    struct c985_buf *buf;

    if (list_empty(&c->ready))
        return NULL;
    buf = list_first_entry(&c->ready, struct c985_buf, list);
    list_del(&buf->list);
    c->queued_count--;
    return buf;
}

/* Render one descriptor into one buffer: 3 synchronous frame-mode DMA reads
 * (Y, U, V=U+0x40), then DQBUFs.
 * Caller holds q_lock for list access but we drop it around the (slow) DMA. */
static void c985_v4l2_render_and_done(struct c985_v4l2 *c,
                                      struct c985_buf *buf,
                                      const struct c985_frame_desc *d)
{
    struct c985_dev *dev = c->dev;
    dma_addr_t base;
    int rc = 0;

    /* Single planar YUV420 buffer: Y then U then V contiguous. The three
     * frame-mode DMAs land at offsets into the one vb2_dma_contig buffer. */
    base = vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);

    if (c985_dma_read_frame_mode(dev, d->y_dw << 2, base,
                                 C985_Y_LEN, C985_WIDTH, false)) {
        rc = -1;
        goto out;
    }
    if (c985_dma_read_frame_mode(dev, d->u_dw << 2, base + C985_Y_LEN,
                                 C985_C_LEN, C985_WIDTH / 2, true)) {
        rc = -1;
        goto out;
    }
    if (c985_dma_read_frame_mode(dev, (d->u_dw << 2) + C985_V_OFFSET_BYTES,
                                 base + C985_Y_LEN + C985_C_LEN,
                                 C985_C_LEN, C985_WIDTH / 2, true))
        rc = -1;

out:
    buf->vb.vb2_buf.timestamp = ktime_get_ns();
    buf->vb.sequence = (u32)atomic64_inc_return(&c->frames_done);
    vb2_set_plane_payload(&buf->vb.vb2_buf, 0, C985_FRAME_BYTES);
    vb2_buffer_done(&buf->vb.vb2_buf,
                    rc ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
}

/* Streaming kthread: blocks on the doorbell waitqueue (woken by the ISR on ARM->host
 * doorbell bit24), reads the mailbox burst, renders into the next queued
 * buffer, and releases the card ring slot via 0x30. */
static int c985_v4l2_thread(void *data)
{
    struct c985_v4l2 *c = data;
    struct c985_dev *dev = c->dev;
    struct c985_frame_desc d;

    while (!kthread_should_stop() && !READ_ONCE(c->thread_stop)) {
        struct c985_buf *buf;
        struct c985_mbox_result *r;
        int ret;

        /* Poll the mailbox (0x6C8 bit0) directly, consume any pending 0x40 frame
         * descriptor, render, and 0x30-release. No doorbell wait — the
         * descriptor is the source of truth. */
        ret = c985_mbox_drain(dev);
        if (ret < 0 && ret != -ENODATA)
            break;
        if (ret == -ENODATA) {
            usleep_range(1000, 2000);
            continue;
        }

        r = &dev->last_mbox;

        /* Only raw-video 0x40 descriptors feed the capture path. */
        if ((r->from_arm_msg & 0xFF) != 0x40) {
            atomic64_inc(&c->desc_non40);
            continue;
        }

        /* Route audio-task (taskId 1) descriptors to the audio consumer. */
        if (((r->from_arm_msg >> 16) & 0xFF) == C985_AUD_TASK) {
            struct c985_frame_desc ad;
            ad.tag = r->resp_params[0] & 0xFFFF;
            ad.ring_idx = (r->resp_params[0] >> 24) & 0xFF;
            ad.y_dw = r->resp_params[1];
            ad.u_dw = r->resp_params[2];
            ad.chroma = r->resp_params[3];
            ad.pts_raw = r->resp_params[4];
            ad.task = C985_AUD_TASK;
            ad.valid = true;
            if (dev->audio_consumer)
                dev->audio_consumer(dev, &ad);
            c985_mbox_release_last(dev);
            continue;
        }

        d.tag = r->resp_params[0] & 0xFFFF;
        d.ring_idx = (r->resp_params[0] >> 24) & 0xFF;
        d.y_dw = r->resp_params[1];
        d.u_dw = r->resp_params[2];
        d.chroma = r->resp_params[3];
        d.pts_raw = r->resp_params[4];
        d.valid = true;

        if (d.y_dw == 0 || d.u_dw == 0 || d.chroma == 0) {
            atomic64_inc(&c->desc_bad);
            continue;
        }

        /* Re-check stop before touching q_lock; stop_streaming holds q_lock
         * while joining us, so a blocking lock here would deadlock. */
        if (kthread_should_stop() || READ_ONCE(c->thread_stop))
            break;

        /* Interruptible: kthread_stop() (during stop_streaming, which holds
         * q_lock) signals the thread; a plain mutex_lock would deadlock
         * against stop_streaming -> kthread_stop waiting for us while we
         * wait on q_lock. */
        if (mutex_lock_interruptible(&c->q_lock))
            break;
        buf = c985_v4l2_next_buf(c);
        mutex_unlock(&c->q_lock);
        if (!buf) {
            atomic64_inc(&c->no_buf);
            continue;
        }

        c985_v4l2_render_and_done(c, buf, &d);

        /* Release the card ring slot. */
        c985_mbox_release_last(dev);
    }

    return 0;
}

void c985_v4l2_get_stats(struct c985_dev *dev, struct c985_stream_stats *s)
{
    struct c985_v4l2 *c = dev->v4l2_priv;

    if (!c) {
        memset(s, 0, sizeof(*s));
        return;
    }
    atomic64_set(&s->frames_done, atomic64_read(&c->frames_done));
    atomic64_set(&s->frames_dropped, atomic64_read(&c->frames_dropped));
    atomic64_set(&s->bytes_done, 0);
    atomic64_set(&s->desc_non40, atomic64_read(&c->desc_non40));
    atomic64_set(&s->desc_bad, atomic64_read(&c->desc_bad));
    atomic64_set(&s->no_buf, atomic64_read(&c->no_buf));
}

/* ---- vb2 callbacks ---- */

static int c985_queue_setup(struct vb2_queue *q,
                            unsigned int *num_buffers, unsigned int *num_planes,
                            unsigned int sizes[], struct device *alloc_devs[])
{
    struct c985_v4l2 *c = q->drv_priv;
    unsigned int nplanes = 1;

    if (*num_planes) {
        if (*num_planes != nplanes || sizes[0] < C985_FRAME_BYTES)
            return -EINVAL;
        return 0;
    }

    *num_planes = nplanes;
    sizes[0] = C985_FRAME_BYTES;

    /* 2 buffers minimum; driver's 16-entry frame FIFO decouples from firmware's 4-slot ring.
     * 30fps = 33ms/frame; 2 buffers recycled every ~66ms feasible for DMA + userspace.
     * Cap at 2 to avoid DMA32 contiguous allocation pressure (3.1MB/buffer). */
    if (*num_buffers < 4)
        *num_buffers = 4;
    if (*num_buffers > VIDEO_MAX_FRAME)
        *num_buffers = VIDEO_MAX_FRAME;

    alloc_devs[0] = &c->dev->pdev->dev;

    return 0;
}

static int c985_buf_prepare(struct vb2_buffer *vb)
{
    struct c985_v4l2 *c = vb->vb2_queue->drv_priv;

    if (vb2_plane_size(vb, 0) < C985_FRAME_BYTES) {
        dev_err(&c->dev->pdev->dev, "buffer too small\n");
        return -EINVAL;
    }
    vb2_set_plane_payload(vb, 0, C985_FRAME_BYTES);
    return 0;
}

static void c985_buf_queue(struct vb2_buffer *vb)
{
    struct c985_v4l2 *c = vb->vb2_queue->drv_priv;
    struct c985_buf *buf = to_c985_buf(vb);

    /* Called with q->lock held by vb2 core. */
    list_add_tail(&buf->list, &c->ready);
    c->queued_count++;
}

static int c985_start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct c985_v4l2 *c = q->drv_priv;
    struct c985_dev *dev = c->dev;
    int ret;

    /* Called with q->lock held by vb2 core. */

    ret = c985_v4l2_boot_encoder(dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "encoder boot chain failed: %d\n", ret);
        return ret;
    }

    /* Boot the audio task (taskId 1) in lockstep. Best-effort: audio may
     * be unavailable (no audio firmware / no signal); don't fail video. */
    if (dev->audio_priv)
        c985_audio_boot(dev);

    /* Reset the mailbox wait state (doorbell may be stale from boot). */
    dev->doorbell_pending = false;

    c->thread_stop = false;
    c->thread = kthread_run(c985_v4l2_thread, c, "c985-v4l2");
    if (IS_ERR(c->thread)) {
        dev_err(&dev->pdev->dev, "failed to start v4l2 thread: %ld\n",
                PTR_ERR(c->thread));
        c->thread = NULL;
        return PTR_ERR(c->thread);
    }

    return 0;
}

static void c985_stop_streaming(struct vb2_queue *q)
{
    struct c985_v4l2 *c = q->drv_priv;
    struct c985_dev *dev = c->dev;
    struct c985_buf *buf, *tmp;

    /* Called with q->lock held by vb2 core. */

    /* Stop the streaming thread first; wake it if it is sleeping. */
    if (c->thread) {
        c->thread_stop = true;
        wake_up_all(&dev->doorbell_wq);
        kthread_stop(c->thread);
        c->thread = NULL;
    }

    /* Stop encoder (0x02), drop 0x40 traffic (fw run-flag clear). */
    c985_v4l2_stop_encoder(dev);

    /* Stop audio task in lockstep. */
    if (dev->audio_priv)
        c985_audio_stop(dev);

    /* Return all still-queued buffers. */
    list_for_each_entry_safe(buf, tmp, &c->ready, list) {
        list_del(&buf->list);
        c->queued_count--;
        vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
    }

    /* Halt ARM to fully quiesce. */
    c985_v4l2_teardown(dev);
    if (dev->audio_priv)
        c985_audio_teardown(dev);
}

/* ---- Format / queue ioctls ---- */

static int c985_querycap(struct file *file, void *fh,
                         struct v4l2_capability *cap)
{
    strscpy(cap->driver, "c985", sizeof(cap->driver));
    strscpy(cap->card, "AverMedia C985", sizeof(cap->card));
    strscpy(cap->bus_info, "PCIe", sizeof(cap->bus_info));
    cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
                       V4L2_CAP_READWRITE;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
}

static int c985_enum_input(struct file *file, void *fh, struct v4l2_input *in)
{
    if (in->index != 0)
        return -EINVAL;

    strscpy(in->name, "HDMI", sizeof(in->name));
    in->type = V4L2_INPUT_TYPE_CAMERA;
    in->std = 0;
    /* No tuner, no standard, no DV timings: OBS skips S_STD/S_DV_TIMINGS. */
    in->capabilities = 0;
    return 0;
}

static int c985_g_input(struct file *file, void *fh, unsigned int *i)
{
    *i = 0;
    return 0;
}

static int c985_s_input(struct file *file, void *fh, unsigned int i)
{
    return i == 0 ? 0 : -EINVAL;
}

static int c985_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
    if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    a->parm.capture.timeperframe.numerator = 1;
    a->parm.capture.timeperframe.denominator = 30;
    return 0;
}

static int c985_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
    if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    return 0;
}

static int c985_enum_fmt_vid_cap(struct file *file, void *fh,
                                 struct v4l2_fmtdesc *f)
{
    if (f->index != 0)
        return -EINVAL;
    f->pixelformat = V4L2_PIX_FMT_YUV420;
    f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return 0;
}

static int c985_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
    struct v4l2_pix_format *pix = &f->fmt.pix;

    if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;

    memset(pix, 0, sizeof(*pix));
    pix->width = C985_WIDTH;
    pix->height = C985_HEIGHT;
    pix->pixelformat = V4L2_PIX_FMT_YUV420;
    pix->field = V4L2_FIELD_NONE;
    pix->bytesperline = C985_WIDTH;
    pix->sizeimage = C985_FRAME_BYTES;
    pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
    return 0;
}

static int c985_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
    if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return -EINVAL;
    /* Fixed format only; rewrite to canonical values. */
    return c985_g_fmt(file, fh, f);
}

static int c985_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
    return c985_try_fmt(file, fh, f);
}

static const struct v4l2_file_operations c985_fops = {
    .owner = THIS_MODULE,
    .open = v4l2_fh_open,
    .release = vb2_fop_release,
    .unlocked_ioctl = video_ioctl2,
    .read = vb2_fop_read,
    .mmap = vb2_fop_mmap,
    .poll = vb2_fop_poll,
};

static const struct v4l2_ioctl_ops c985_ioctl_ops = {
    .vidioc_querycap = c985_querycap,
    .vidioc_enum_input = c985_enum_input,
    .vidioc_g_input = c985_g_input,
    .vidioc_s_input = c985_s_input,
    .vidioc_g_parm = c985_g_parm,
    .vidioc_s_parm = c985_s_parm,
    .vidioc_enum_fmt_vid_cap = c985_enum_fmt_vid_cap,
    .vidioc_g_fmt_vid_cap = c985_g_fmt,
    .vidioc_try_fmt_vid_cap = c985_try_fmt,
    .vidioc_s_fmt_vid_cap = c985_s_fmt,
    .vidioc_reqbufs = vb2_ioctl_reqbufs,
    .vidioc_create_bufs = vb2_ioctl_create_bufs,
    .vidioc_prepare_buf = vb2_ioctl_prepare_buf,
    .vidioc_querybuf = vb2_ioctl_querybuf,
    .vidioc_qbuf = vb2_ioctl_qbuf,
    .vidioc_dqbuf = vb2_ioctl_dqbuf,
    .vidioc_expbuf = vb2_ioctl_expbuf,
    .vidioc_streamon = vb2_ioctl_streamon,
    .vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct vb2_ops c985_vb2_ops = {
    .queue_setup = c985_queue_setup,
    .buf_prepare = c985_buf_prepare,
    .buf_queue = c985_buf_queue,
    .start_streaming = c985_start_streaming,
    .stop_streaming = c985_stop_streaming,
};

/* ---- Encoder boot chain (raw-profile, verified vs AVerPL33_x64.sys) ----
 * F1 SystemOpen -> F2 SystemLink -> six 0x10 setparams -> 0x01 StartEncoder
 * with the raw-mode config block. All silent (no completion wait).
 * task_id selects video (0) or audio (1); params are task-specific. */

struct c985_enc_step {
    u16 opcode;
    u32 param0;
    unsigned long timeout_ms;
    u32 slots[3];
    int nslots;
};

static int c985_enc_send(struct c985_dev *dev, u8 task_id, u16 opcode,
                         u32 param0, unsigned long to, const u32 *slots,
                         int nslots, bool bare)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(dev->mbox_slots); i++)
        dev->mbox_slots[i] = (i < nslots) ? slots[i] : 0;

    return c985_mbox_send_polling(dev, opcode, param0, task_id,
                                  false, bare, to);
}

/* Direct BAR1 write of the encoder config block, descending 0x6F8..0x6D0,
 * matching Windows CQLCodec_UpdateEncoderConfig's QPFWENCAPI_Set* burst
 * (each setter is a bare RegisterWrite, no mailbox message). The subsequent
 * UpdateConfig 0x06 commits this burst to firmware. */

void c985_enc_write_config(struct c985_dev *dev, const u32 *cfg)
{
    int i;

    for (i = 0; i < C985_ENC_CFG_REGS; i++)
        c985_write_bar1(dev, C985_TO_ARM_PARAM0 - i * 4, cfg[i]);
    wmb();
}

int c985_v4l2_boot_encoder(struct c985_dev *dev)
{
    static const struct c985_enc_step steps[] = {
        { 0xF1, 0x80000004, 300, { 0 }, 0 },
        { 0xF2, 0x1080100, 300, { 0 }, 0 },
        { 0x10, 0x0F,       200, { 0, 0 }, 2 },
        { 0x10, 0x10,       200, { 0, 0, 0 }, 3 },
        { 0x10, 0x12,       200, { 0 }, 1 },
        { 0x10, 0x13,       200, { 80, 0, 10 }, 3 },
        { 0x10, 0x14,       200, { 1, 0x4A38 }, 2 },
        { 0x10, 0x02,       200, { 0xF1F1F1DA, 0xB6F1F1B6 }, 2 },
    };
    u32 picres = (C985_HEIGHT << 16) | C985_WIDTH;
    /* Raw-profile config block, member order 0x6F8..0x6D0:
     * SysCtrl, PicRes, InCtrl, RateCtrl, BitRate, Filter, GOPLF,
     * ET(=picres: W/H for VDCM ring geometry), BlockSize, OutPicRes,
     * AudioControlEx. */
    u32 cfg[C985_ENC_CFG_REGS] = {
        0x2101b20c, picres, 0x0f7c0609, 0x005003e8, 0x1f4007d0,
        0x80002000, 0x6001000a, picres, 0x10, 0x01121080, 0x200,
    };
    int i, ret;

    for (i = 0; i < ARRAY_SIZE(steps); i++) {
        ret = c985_enc_send(dev, C985_ENC_TASK, steps[i].opcode,
                            steps[i].param0, steps[i].timeout_ms,
                            steps[i].slots, steps[i].nslots, false);
        if (ret && ret != -ETIMEDOUT) {
            dev_err(&dev->pdev->dev, "boot step 0x%02x failed: %d\n",
                    steps[i].opcode, ret);
            return ret;
        }
    }

    /* Windows: CQLCodec_Set -> register burst 0x6F8..0x6D0, then commit
     * with UpdateConfig 0x06 (bare), then bare StartEncoder 0x01. */
    c985_enc_write_config(dev, cfg);

    ret = c985_enc_send(dev, C985_ENC_TASK, 0x06, C985_ENC_TASK, 300,
                        NULL, 0, true);
    if (ret && ret != -ETIMEDOUT)
        return ret;

    return c985_enc_send(dev, C985_ENC_TASK, 0x01, C985_ENC_TASK, 300,
                         NULL, 0, true);
}

void c985_v4l2_stop_encoder(struct c985_dev *dev)
{
    /* 0x02 StopEncoder (bStopAtGOP=0). Clears the task run-flag so the
     * firmware DTM drops subsequent 0x40 traffic. */
    c985_enc_send(dev, C985_ENC_TASK, 0x02, 0, 500, NULL, 0, false);
}

void c985_v4l2_teardown(struct c985_dev *dev)
{
    /* 0xF3 SystemClose full session teardown.
     * No ARM halt: keep firmware booted so a subsequent stream-open can
     * re-issue F1/F2 without a full re-upload. */
    c985_enc_send(dev, C985_ENC_TASK, 0xF3, 0, 500, NULL, 0, false);
    msleep(100);
}

int c985_v4l2_init(struct c985_dev *dev)
{
    struct c985_v4l2 *c;
    struct vb2_queue *q;
    int ret;

    c = devm_kzalloc(&dev->pdev->dev, sizeof(*c), GFP_KERNEL);
    if (!c)
        return -ENOMEM;
    c->dev = dev;
    dev->v4l2_priv = c;

    mutex_init(&c->q_lock);
    INIT_LIST_HEAD(&c->ready);
    atomic64_set(&c->frames_done, 0);
    atomic64_set(&c->frames_dropped, 0);
    atomic64_set(&c->desc_non40, 0);
    atomic64_set(&c->desc_bad, 0);
    atomic64_set(&c->no_buf, 0);

    snprintf(c->v4l2_dev.name, sizeof(c->v4l2_dev.name), "%s-%s",
             "c985", pci_name(dev->pdev));
    ret = v4l2_device_register(&dev->pdev->dev, &c->v4l2_dev);
    if (ret)
        goto err;

    q = &c->queue;
    q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    q->io_modes = VB2_MMAP | VB2_READ;
    q->drv_priv = c;
    q->buf_struct_size = sizeof(struct c985_buf);
    q->ops = &c985_vb2_ops;
    q->mem_ops = &vb2_dma_contig_memops;
    q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    q->min_queued_buffers = 2;
    q->lock = &c->q_lock;
    q->dev = &dev->pdev->dev;

    ret = vb2_queue_init(q);
    if (ret)
        goto err_v4l2;

    c->vdev.queue = q;
    c->vdev.queue->lock = &c->q_lock;
    c->vdev.fops = &c985_fops;
    c->vdev.ioctl_ops = &c985_ioctl_ops;
    c->vdev.v4l2_dev = &c->v4l2_dev;
    c->vdev.vfl_dir = VFL_DIR_RX;   /* capture (RX) */
    c->vdev.release = video_device_release_empty;
    c->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
                          V4L2_CAP_READWRITE;
    strscpy(c->vdev.name, C985_V4L2_NAME, sizeof(c->vdev.name));
    video_set_drvdata(&c->vdev, dev);

    ret = video_register_device(&c->vdev, VFL_TYPE_VIDEO, -1);
    if (ret)
        goto err_v4l2;

    dev_info(&dev->pdev->dev, "v4l2 device registered as %s\n",
             video_device_node_name(&c->vdev));
    return 0;

err_v4l2:
    v4l2_device_unregister(&c->v4l2_dev);
err:
    dev->v4l2_priv = NULL;
    devm_kfree(&dev->pdev->dev, c);
    return ret;
}

void c985_v4l2_cleanup(struct c985_dev *dev)
{
    struct c985_v4l2 *c = dev->v4l2_priv;

    if (!c)
        return;

    if (c->thread) {
        c->thread_stop = true;
        wake_up_all(&dev->doorbell_wq);
        kthread_stop(c->thread);
        c->thread = NULL;
    }

    video_unregister_device(&c->vdev);
    v4l2_device_unregister(&c->v4l2_dev);

    dev->v4l2_priv = NULL;
    devm_kfree(&dev->pdev->dev, c);
}
