// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/* ---- Mailbox Communication (polling mode) ----
 *
 * Firmware protocol (Ghidra-verified against qpvidfwpcie.bin):
 *   - 0x6FC bits[15:0] = opcode, bits[31:16] = taskId (ISR decodes LOW half;
 *     0x00F20000 decodes as type 0x0000 and is silently swallowed!)
 *   - 0x6F8 = payload word 0 (function/link params)
 *   - 0x6CC = status word (taskId<<16 | hasResp<<8 | 1) - consumed by poll
 *   - System commands (0xF1/0xF2) route via firmware's own default queue
 *   - Responses: DTM thread writes 0x6C8=(code<<16)|1 first, params
 *     0x6B4-0x6C0, msg 0x6B0, BAR1+0xB30=0x80000000, doorbell bit24 last.
 *     It SPINS while 0x6C8 bit0 is set, so host MUST clear it after reading,
 *     and send ack word 0x31/0xA2|(msg&0xFFFF0000) when bit8 was set.
 */

static int c985_mbox_wait_free(struct c985_dev *dev, unsigned long timeout_ms)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

    while (c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS) & 1) {
        if (time_after(jiffies, deadline)) {
            dev_err(&dev->pdev->dev,
                    "mbox: 0x6CC busy (0x%08x), previous command never consumed\n",
                    c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS));
            return -EBUSY;
        }
        usleep_range(100, 500);
    }
    return 0;
}

static void c985_mbox_post(struct c985_dev *dev, u32 status_word, u32 msg_word)
{
    c985_write_bar1(dev, C985_TO_ARM_MSG_STATUS, status_word);
    wmb();
    c985_write_bar1(dev, C985_TO_ARM_MESSAGE, msg_word);
    wmb();
    /* Kick ARM via doorbell (host->ARM bit 25) per Windows driver MM_SetInterrupt */
    c985_write_bar1(dev, C985_DOORBELL, 0x02000000);
}

int c985_mbox_send_polling(struct c985_dev *dev, u16 opcode, u32 param0,
                           u8 task_id, bool has_resp, bool bare,
                           unsigned long timeout_ms)
{
    unsigned long deadline;
    ktime_t t0;
    s64 elapsed_us;
    struct c985_mbox_result *r = &dev->last_mbox;
    int ret;

    memset(r, 0, sizeof(*r));
    r->clear_us = -1;
    r->resp_us = -1;

    /* Interruptible: a stuck holder must be breakable with Ctrl-C,
     * otherwise one bug bricks the whole module */
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Log BEFORE touching registers: if the card wedges a PCIe transaction,
     * this line marks the entry point that never returned */
    dev_dbg(&dev->pdev->dev,
             "mbox_send: begin opcode=0x%02x param=0x%08x task=%u hasResp=%d\n",
             opcode, param0, task_id, has_resp);

    ret = c985_mbox_wait_free(dev, timeout_ms);
    if (ret) {
        mutex_unlock(&dev->lock);
        return ret;
    }

    /* Payload window first (descending 0x6F8..0x6D0), then status+message
     * (firmware down-walks the payload window on consume - Windows writes
     * the full config block immediately before StartEncoder). bare=true
     * matches Windows QPFWAPI_SendMessageToARM: write ONLY 0x6CC+0x6FC (no
     * 0x6F8, no payload window) so a pre-written register burst survives. */
    r->param0_written = param0;
    r->task_id = task_id;
    if (opcode == 0x30) {
        dev_dbg(&dev->pdev->dev, "mbox_send 0x30: mbox_slots[4]=0x%08x\n", dev->mbox_slots[4]);
        r->status_sent = (dev->mbox_slots[4] << 20) | 1u;
    } else {
        r->status_sent = ((u32)task_id << 16) | ((has_resp ? 1u : 0u) << 8) | 1u;
    }
    r->msg_written = ((u32)task_id << 16) | opcode;

    if (!bare) {
        c985_write_bar1(dev, C985_TO_ARM_PARAM0, param0);
        if (opcode == 0x11 || opcode == 0x06 || opcode == 0x01 ||
            opcode == 0x12 || opcode == 0x09 || opcode == 0x10 ||
            opcode == 0x30 || opcode == 0x81 || opcode == 0x90 ||
            opcode == 0xA0 || opcode == 0xA1) {
            static const u16 slot_off[ARRAY_SIZE(dev->mbox_slots)] = {
                0x6F4, 0x6F0, 0x6EC, 0x6E8, 0x6E4,
                0x6E0, 0x6DC, 0x6D8, 0x6D4, 0x6D0,
            };
            int i;

            for (i = 0; i < ARRAY_SIZE(slot_off); i++)
                c985_write_bar1(dev, slot_off[i], dev->mbox_slots[i]);
            wmb();
        }
    }
    wmb();
    c985_mbox_post(dev, r->status_sent, r->msg_written);

    dev_dbg(&dev->pdev->dev,
             "mbox_send: opcode=0x%02x param=0x%08x task=%u status=0x%08x msg=0x%08x\n",
             opcode, param0, task_id, r->status_sent, r->msg_written);

    /* Poll for ARM consumption */
    t0 = ktime_get();
    deadline = jiffies + msecs_to_jiffies(timeout_ms);
    while (c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS) & 1) {
        if (time_after(jiffies, deadline)) {
            dev_err(&dev->pdev->dev,
                    "mbox_send: TIMEOUT - ARM did not consume (%lu ms)\n",
                    timeout_ms);
            c985_boot_diag(dev, "mbox-consume-timeout");
            dev->have_mbox_result = true;
            mutex_unlock(&dev->lock);
            return -ETIMEDOUT;
        }
        usleep_range(50, 200);
    }
    elapsed_us = ktime_us_delta(ktime_get(), t0);
    r->clear_us = elapsed_us;
    dev_dbg(&dev->pdev->dev, "mbox_send: ARM consumed command in %lld us\n",
             elapsed_us);

    if (!has_resp)
        goto out;

    /* Wait for response: DTM thread sets 0x6C8 bit0 */
    t0 = ktime_get();
    deadline = jiffies + msecs_to_jiffies(timeout_ms);
    while (!(c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS) & 1)) {
        if (time_after(jiffies, deadline)) {
            dev_warn(&dev->pdev->dev,
                     "mbox_send: no response within %lu ms\n", timeout_ms);
            c985_boot_diag(dev, "no-response");
            c985_task_state_dump(dev, task_id);
            goto out;
        }
        usleep_range(50, 200);
    }
    r->resp_us = ktime_us_delta(ktime_get(), t0);
    r->resp_ok = true;

    /* DTM sets 0x6C8 bit0 BEFORE writing the response window - settle
     * before reading or params come back zero */
    usleep_range(100, 200);

    /* Read full response */
    r->from_arm_msg = c985_read_bar1(dev, C985_FROM_ARM_MESSAGE);
    r->from_arm_status = c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS);
    for (ret = 0; ret < 5; ret++)
        r->resp_params[ret] = c985_read_bar1(dev, 0x6B4 + ret * 4);

    dev_dbg(&dev->pdev->dev,
             "mbox_send: response msg=0x%08x status=0x%08x p0..p4=%08x %08x %08x %08x %08x\n",
             r->from_arm_msg, r->from_arm_status,
             r->resp_params[0], r->resp_params[1], r->resp_params[2],
             r->resp_params[3], r->resp_params[4]);

    /* Ack: always clear bit0 (DTM spins while set); if bit8 was set,
     * send ack message 0x31/0xA2 with original taskId in high half */
    {
        u32 st = r->from_arm_status;
        bool need_ack = (st & 0x100) != 0;

        st &= ~1u;
        c985_write_bar1(dev, C985_FROM_ARM_MSG_STATUS, st);

        if (need_ack) {
            u32 ack_op = (r->from_arm_msg & 0xFFFF) < 0x80 ? 0x31 : 0xA2;

            r->ack_word = (r->from_arm_msg & 0xFFFF0000) | ack_op;
            ret = c985_mbox_wait_free(dev, 500);
            if (!ret) {
                c985_write_bar1(dev, C985_TO_ARM_PARAM0, 0);
                wmb();
                c985_mbox_post(dev, (r->ack_word & 0xFFFF0000) | 1u,
                               r->ack_word);
                dev_dbg(&dev->pdev->dev, "mbox_send: ack sent 0x%08x\n",
                         r->ack_word);
                /* Best-effort consume wait so mailbox is clean afterwards */
                deadline = jiffies + msecs_to_jiffies(200);
                while ((c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS) & 1) &&
                       time_before(jiffies, deadline))
                    usleep_range(50, 200);
            }
        }
    }

out:
    if (!r->resp_ok) {
        /* Capture live state for diagnosis without clobbering a real
         * response already stored above */
        r->from_arm_msg = c985_read_bar1(dev, C985_FROM_ARM_MESSAGE);
        r->from_arm_status = c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS);
    }
    dev->have_mbox_result = true;

    /* Auto-release: Windows sends opcode 0x30 (CompleteArm) after every
     * consumed 0x40 frame descriptor - without it the firmware buffer
     * pool starves and the encoder stalls */
    {
        bool do_release = dev->auto_release && !dev->releasing &&
                          r->resp_ok &&
                          (r->from_arm_msg & 0xFF) == 0x40;

        mutex_unlock(&dev->lock);
        if (do_release)
            c985_mbox_release_last(dev);
        return has_resp && !r->resp_ok ? -ETIMEDOUT : 0;
    }
}

/* Send CompleteArm (0x30) for the descriptor currently in last_mbox.
 * Encoding per CTask_CompleteArm (AVerPL33_x64.sys):
 *   0x6F8=tag  0x6F4=size DWORDS  0x6F0=PTS&0x7fffffff
 *   0x6EC=PTSValid(bit31 of w5)  0x6E4=ring index (w1>>24)
 *   0x6CC=(orig status & 0xFFFF0000)|1  0x6FC=(taskId<<16)|0x30 */
void c985_mbox_release_last(struct c985_dev *dev)
{
    u32 saved[sizeof(dev->mbox_slots) / sizeof(u32)];
    u32 tag, size_dw, pts, pts_valid, ring_idx, status;
    struct c985_mbox_result *r = &dev->last_mbox;

    if (dev->releasing)
        return;
    dev->releasing = true;

    tag = r->resp_params[0] & 0xFFFF;
    /* Audio (tag 0x82 raw LPCM) descriptor carries size already in DWORDs
     * (resp_params[3]); video (0x81 raw YUV) carries chroma-plane BYTES, so
     * total Y+U+V = 3*chroma/2 bytes, then /4 -> DWORDS = 3*chroma/8... but
     * dbgview shows video release 0x6F4 = 0xbdd80 constant (777088 DW). For
     * audio, 0x6F4 = resp_params[3] directly (dbgview: 0x480 for 4608B). */
    if (tag == C985_DT_RAW_AUDIO) {
        size_dw = r->resp_params[3];      /* already DWORDs */
    } else {
        size_dw = (r->resp_params[3] * 3) / 2; /* video: Y+U+V DWORDS */
    }
    pts = r->resp_params[4] & 0x7FFFFFFFu;
    pts_valid = (r->resp_params[4] & 0x80000000u) ? 1 : 0;
    ring_idx = (r->resp_params[0] >> 24) & 0xFF;
    status = (r->from_arm_status & 0xFFFF0000u) | 1u;

    memcpy(saved, dev->mbox_slots, sizeof(saved));
    memset(dev->mbox_slots, 0, sizeof(saved));
    dev->mbox_slots[0] = size_dw;    /* 0x6F4: total frame size in DWORDs */
    dev->mbox_slots[1] = pts;        /* 0x6F0 */
    dev->mbox_slots[2] = pts_valid;  /* 0x6EC */
    dev->mbox_slots[4] = ring_idx;   /* 0x6E4 (slot3 0x6E8 left 0) */

    c985_mbox_send_polling(dev, 0x30, tag, r->task_id, false, false, 200);

    memcpy(dev->mbox_slots, saved, sizeof(saved));
    dev->releasing = false;
}

/* Poll for an unsolicited 0x40, capture it into last_mbox and consume it.
 * Without this, a frame-done arriving between commands leaves 0x6C8 bit0
 * set and the DTM thread spins forever (mailbox wedges). */
int c985_mbox_drain(struct c985_dev *dev)
{
    struct c985_mbox_result *r = &dev->last_mbox;
    u32 st;
    int ret = -ENODATA;
    bool do_release = false;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    st = c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS);
    if (st & 1) {
        /* DTM sets 0x6C8 bit0 BEFORE writing params/0x6B0 - let the
         * register burst finish or we read zeros mid-update */
        usleep_range(100, 200);
        memset(r, 0, sizeof(*r));
        r->clear_us = 0;
        r->resp_us = 0;
        r->resp_ok = true;
        r->from_arm_msg = c985_read_bar1(dev, C985_FROM_ARM_MESSAGE);
        r->from_arm_status = st;
        r->task_id = (r->from_arm_msg >> 16) & 0xFF;
        for (u32 i = 0; i < 5; i++)
            r->resp_params[i] = c985_read_bar1(dev, 0x6B4 + i * 4);

        c985_write_bar1(dev, C985_FROM_ARM_MSG_STATUS, st & ~1u);
        dev->have_mbox_result = true;
        dev_dbg(&dev->pdev->dev,
                 "mbox_drain: msg=0x%08x st=0x%08x p0..p4=%08x %08x %08x %08x %08x\n",
                 r->from_arm_msg, r->from_arm_status,
                 r->resp_params[0], r->resp_params[1], r->resp_params[2],
                 r->resp_params[3], r->resp_params[4]);
        do_release = dev->auto_release && !dev->releasing &&
                     (r->from_arm_msg & 0xFF) == 0x40;
        ret = 0;
    }

    mutex_unlock(&dev->lock);

    if (do_release)
        c985_mbox_release_last(dev);
    return ret;
}

/* Wait for ARM->host doorbell (bit 24), then read mailbox regardless of
 * 0x6C8 bit 0 state. ARM signals frame-done via doorbell but doesn't
 * always set the "response required" bit in 0x6C8. */
int c985_mbox_wait_and_read(struct c985_dev *dev, unsigned long timeout_ms)
{
    struct c985_mbox_result *r = &dev->last_mbox;
    int ret;

    ret = wait_event_interruptible_timeout(dev->doorbell_wq,
                                           dev->doorbell_pending,
                                           msecs_to_jiffies(timeout_ms));
    if (ret <= 0)
        return ret == 0 ? -ETIMEDOUT : ret;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* Clear the pending flag */
    dev->doorbell_pending = false;

    /* DTM sets doorbell BEFORE writing params/0x6B0 - let the
     * register burst finish or we read zeros mid-update (same as mbox_drain) */
    usleep_range(100, 200);

    /* Read full mailbox state (doorbell already cleared by ISR) */
    memset(r, 0, sizeof(*r));
    r->clear_us = 0;
    r->resp_us = 0;
    r->resp_ok = true;
    r->from_arm_msg = c985_read_bar1(dev, C985_FROM_ARM_MESSAGE);
    r->from_arm_status = c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS);
    r->task_id = (r->from_arm_msg >> 16) & 0xFF;
    for (u32 i = 0; i < 5; i++)
        r->resp_params[i] = c985_read_bar1(dev, 0x6B4 + i * 4);

    dev->have_mbox_result = true;
    dev_dbg(&dev->pdev->dev,
             "mbox_wait: msg=0x%08x st=0x%08x p0..p4=%08x %08x %08x %08x %08x\n",
             r->from_arm_msg, r->from_arm_status,
             r->resp_params[0], r->resp_params[1], r->resp_params[2],
             r->resp_params[3], r->resp_params[4]);

    mutex_unlock(&dev->lock);
    return 0;
}

/* ---- Interrupt-driven frame FIFO (v4l2 streaming path) ----
 *
 * Replaces polling (mbox_drain debugfs + 20ms sleep)
 * with: ISR detects ARM->host doorbell bit24 -> c985_mbox_isr_service reads
 * the 7-word mailbox burst, clears 0x6C8 bit0 (unblock DTM), and if the
 * opcode is 0x40 pushes a c985_frame_desc onto the FIFO and schedules the
 * drain work. The drain work pops descriptors and hands each to the v4l2
 * layer, which renders the Y/U/V planes via synchronous frame-mode DMA.
 */

bool c985_mbox_fifo_push(struct c985_dev *dev, struct c985_frame_desc *d)
{
    struct c985_frame_fifo *f = &dev->frame_fifo;
    unsigned long flags;
    bool ok = false;

    spin_lock_irqsave(&f->lock, flags);
    atomic_inc(&f->frames);
    if (f->count < C985_FRAME_FIFO_DEPTH) {
        f->ring[f->head] = *d;
        f->head = (f->head + 1) % C985_FRAME_FIFO_DEPTH;
        f->count++;
        ok = true;
    } else {
        atomic_inc(&f->overflow);
    }
    spin_unlock_irqrestore(&f->lock, flags);
    return ok;
}

bool c985_mbox_fifo_pop(struct c985_dev *dev, struct c985_frame_desc *d)
{
    struct c985_frame_fifo *f = &dev->frame_fifo;
    unsigned long flags;
    bool ok = false;

    spin_lock_irqsave(&f->lock, flags);
    if (f->count) {
        *d = f->ring[f->tail];
        f->tail = (f->tail + 1) % C985_FRAME_FIFO_DEPTH;
        f->count--;
        ok = true;
    }
    spin_unlock_irqrestore(&f->lock, flags);
    return ok;
}

/* ISR-side service: read the mailbox burst and, for opcode 0x40, extract a
 * frame descriptor. Must NOT sleep. Returns true if a 0x40 was queued. */
void c985_mbox_isr_service(struct c985_dev *dev)
{
    u32 msg, status;
    struct c985_frame_desc d;
    u32 p[5];
    int i;

    /* DTM sets 0x6C8 bit0 and the params BEFORE the doorbell; the ISR runs
     * slightly after, so a tiny settle is normally unnecessary, but the burst
     * may still be mid-flight on the same bus clock - read param regs after
     * the status. We read status first, then params, then clear bit0. */
    status = c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS);
    msg = c985_read_bar1(dev, C985_FROM_ARM_MESSAGE);
    for (i = 0; i < 5; i++)
        p[i] = c985_read_bar1(dev, 0x6B4 + i * 4);

    /* Always clear 0x6C8 bit0 so DTM doesn't wedge (it spins on bit0). */
    if (status & 1)
        c985_write_bar1(dev, C985_FROM_ARM_MSG_STATUS, status & ~1u);

    /* Ack message if bit8 was set (mirrors polling path ack logic). */
    if (status & 0x100) {
        u32 ack_op = (msg & 0xFFFF) < 0x80 ? 0x31 : 0xA2;
        u32 ack_word = (msg & 0xFFFF0000) | ack_op;
        u8 task = (msg >> 16) & 0xFF;
        /* Best-effort ack; mailbox may be busy - skip if so. */
        if (!(c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS) & 1)) {
            c985_write_bar1(dev, C985_TO_ARM_PARAM0, 0);
            c985_write_bar1(dev, C985_TO_ARM_MSG_STATUS,
                            (ack_word & 0xFFFF0000) | 1u);
            c985_write_bar1(dev, C985_TO_ARM_MESSAGE,
                            ((u32)task << 16) | ack_op);
            c985_write_bar1(dev, C985_DOORBELL, 0x02000000);
        }
    }

    /* Only 0x40/0x41 (EncDataOutReq frame-done) feeds the frame FIFO. */
    if ((msg & 0xFF) != 0x40 && (msg & 0xFF) != 0x41)
        return;

    d.tag = p[0] & 0xFFFF;
    d.ring_idx = (p[0] >> 24) & 0xFF;
    d.y_dw = p[1];
    d.u_dw = p[2];
    d.chroma = p[3];
    d.pts_raw = p[4];
    d.task = (msg >> 16) & 0xFF;
    d.valid = true;

    if (c985_mbox_fifo_push(dev, &d))
        queue_work(dev->mbox_drain_wq, &dev->mbox_drain_work);
}

/* Workqueue consumer: drain FIFO, hand each descriptor to the appropriate
 * layer. taskId 0 = video (frame_consumer), taskId 1 = audio
 * (audio_consumer). */
void c985_mbox_drain_work_fn(struct work_struct *w)
{
    struct c985_dev *dev = container_of(w, struct c985_dev, mbox_drain_work);
    struct c985_frame_desc d;

    while (c985_mbox_fifo_pop(dev, &d)) {
        if (!d.valid)
            continue;
        if (d.task == C985_AUD_TASK) {
            if (dev->audio_consumer)
                dev->audio_consumer(dev, &d);
            else
                dev_warn_ratelimited(&dev->pdev->dev,
                    "0x40 audio frame dropped: no audio_consumer registered\n");
            continue;
        }
        if (dev->frame_consumer)
            dev->frame_consumer(dev, &d);
        else
            dev_warn_ratelimited(&dev->pdev->dev,
                "0x40 frame dropped: no frame_consumer registered\n");
    }
}
