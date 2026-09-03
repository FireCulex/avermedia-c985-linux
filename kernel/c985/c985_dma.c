// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/* ---- DMA Engine (matches legacy driver PedDmaInit/PedDmaQueueBuffers) ---- */

int c985_dma_init(struct c985_dev *dev)
{
    int i;
    u32 caps, max_transfer = 0;

    for (i = 0; i < 64; i++) {
        dev->dma_chans[i].regs = dev->bar0 + C985_DMA_CHAN_BASE + i * C985_DMA_CHAN_STRIDE;
        dev->dma_chans[i].chan_id = i;
        init_completion(&dev->dma_chans[i].done);
        dev->dma_chans[i].in_use = false;
    }

    dev->dma_write_chan = -1;
    dev->dma_read_chan = -1;

    /* Enable PCIe global IRQ (legacy driver does this in CPCIeCntl_EnableInterrupts) */
    iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);

    /* Scan all 64 possible DMA engines */
    for (i = 0; i < 64; i++) {
        void __iomem *engine_regs = dev->bar0 + C985_DMA_CHAN_BASE + i * C985_DMA_CHAN_STRIDE;

        caps = ioread32(engine_regs + C985_DMA_REG_CAPS);

        /* Skip if not present */
        if (!(caps & 0x01))
            continue;
        if (caps == 0xFFFFFFFF)
            continue;

        dev_dbg(&dev->pdev->dev, "DMA[%d]: caps=0x%08x\n", i, caps);

        u32 engine_max = 1 << ((caps >> 16) & 0x1F);
        if (engine_max > max_transfer)
            max_transfer = engine_max;

        bool is_c2s = (caps & 0x02) != 0;

        /* Enable this engine */
        iowrite32(1, engine_regs + C985_DMA_REG_CTRL);

        /* Track first read/write channel */
        if (is_c2s) {  /* C2S = Card-to-System = read */
            if (dev->dma_read_chan == -1) {
                dev->dma_read_chan = i;
                dev->dma_chans[i].in_use = true;
                dev_dbg(&dev->pdev->dev, "DMA[%d]: read channel (C2S)\n", i);
            }
        } else {  /* S2C = System-to-Card = write */
            if (dev->dma_write_chan == -1) {
                dev->dma_write_chan = i;
                dev->dma_chans[i].in_use = true;
                dev_dbg(&dev->pdev->dev, "DMA[%d]: write channel (S2C)\n", i);
            }
        }
    }

    if (dev->dma_write_chan == -1)
        dev->dma_write_chan = 0;
    if (dev->dma_read_chan == -1)
        dev->dma_read_chan = 1;

    if (!dev->dma_chans[dev->dma_write_chan].in_use)
        dev->dma_chans[dev->dma_write_chan].in_use = true;
    if (!dev->dma_chans[dev->dma_read_chan].in_use)
        dev->dma_chans[dev->dma_read_chan].in_use = true;

    dev_dbg(&dev->pdev->dev, "DMA: write_chan=%d read_chan=%d, max_transfer=0x%x\n",
             dev->dma_write_chan, dev->dma_read_chan, max_transfer);
    return 0;
}

int c985_dma_alloc_desc(struct c985_dev *dev, int chan_id, int num_descs)
{
    struct c985_dma_chan *chan = &dev->dma_chans[chan_id];
    /* Pad so the 32-byte-aligned descriptor inside never exceeds the alloc */
    size_t size = num_descs * sizeof(struct c985_dma_desc) + C985_DMA_DESC_ALIGN;

    chan->desc_ring = dma_alloc_coherent(&dev->pdev->dev, size,
                                         &chan->desc_ring_phys, GFP_KERNEL);
    if (!chan->desc_ring)
        return -ENOMEM;

    memset(chan->desc_ring, 0, size);
    chan->desc_count = num_descs;
    dev_dbg(&dev->pdev->dev, "DMA[%d]: allocated %d descs at 0x%pad\n",
             chan_id, num_descs, &chan->desc_ring_phys);
    return 0;
}

void c985_dma_free_desc(struct c985_dev *dev, int chan_id)
{
    struct c985_dma_chan *chan = &dev->dma_chans[chan_id];
    if (chan->desc_ring) {
        size_t size = chan->desc_count * sizeof(struct c985_dma_desc) +
                      C985_DMA_DESC_ALIGN;
        dma_free_coherent(&dev->pdev->dev, size, chan->desc_ring,
                          chan->desc_ring_phys);
        chan->desc_ring = NULL;
        chan->desc_count = 0;
    }
}

int c985_dma_submit(struct c985_dev *dev, int chan_id,
                    dma_addr_t host_addr, u32 card_addr, u32 len, bool write)
{
    struct c985_dma_chan *chan = &dev->dma_chans[chan_id];
    struct c985_dma_desc *desc = chan->desc_ring;
    void __iomem *engine_regs = chan->regs;
    u32 ctrl;
    u64 card_addr_full;
    dma_addr_t desc_phys_aligned;

    if (!chan->in_use || !chan->desc_ring)
        return -EINVAL;

    reinit_completion(&chan->done);

    /* Enable global DMA if needed */
    if ((ioread32(dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL) & 1) == 0) {
        iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);
    }

    /* Check engine is idle */
    if (ioread32(engine_regs + C985_DMA_REG_CTRL) & C985_DMA_STATUS_BUSY) {
        dev_err(&dev->pdev->dev, "DMA engine %d busy\n", chan_id);
        return -EBUSY;
    }

    /* Align descriptor address to 32 bytes */
    desc_phys_aligned = (chan->desc_ring_phys + 31) & ~31;

    /* Control per transfer direction (PedDmaQueueBuffers@0xB8C60):
     * linear write 0x0C02100F, linear read 0x0804200F */
    ctrl = write ? C985_DMA_CTRL_LIN_WRITE : C985_DMA_CTRL_LIN_READ;

    /* Card address: base flag always set; reads mark the end of the chain
     * with bit59 on the LAST descriptor's card address (no dummy desc) */
    card_addr_full = (u64)card_addr | C985_DMA_CARD_FLAG;
    if (!write)
        card_addr_full |= C985_DMA_CARD_RD_END;

    /* Build main descriptor */
    desc->ctrl = cpu_to_le32(ctrl);
    desc->len = cpu_to_le32(len);
    desc->host_addr = cpu_to_le64(host_addr);
    desc->card_addr = cpu_to_le64(card_addr_full);

    if (write) {
        /* Chain to dummy descriptor */
        desc->next_desc = cpu_to_le64(desc_phys_aligned + sizeof(struct c985_dma_desc));

        /* Build dummy descriptor for writes */
        struct c985_dma_desc *dummy = desc + 1;
        dummy->ctrl = cpu_to_le32(ctrl);
        dummy->len = cpu_to_le32(4);
        dummy->host_addr = cpu_to_le64(host_addr);
        dummy->card_addr = cpu_to_le64(0x0800000000000000ULL);
        dummy->next_desc = cpu_to_le64(0);
    } else {
        desc->next_desc = cpu_to_le64(0);
    }

    /* Memory barrier */
    wmb();

    /* Write descriptor pointer to hardware */
    iowrite32(lower_32_bits(desc_phys_aligned), engine_regs + C985_DMA_REG_DESC_LO);
    iowrite32(upper_32_bits(desc_phys_aligned), engine_regs + C985_DMA_REG_DESC_HI);

    /* Start DMA: 0x101 */
    iowrite32(C985_DMA_CTRL_START, engine_regs + C985_DMA_REG_CTRL);

    dev_dbg(&dev->pdev->dev, "DMA[%d] submit: host=0x%llx card=0x%llx len=%u write=%d\n",
            chan_id, (unsigned long long)host_addr,
            (unsigned long long)card_addr_full, len, write);

    return 0;
}

int c985_dma_wait(struct c985_dev *dev, int chan_id, unsigned long timeout_ms)
{
    struct c985_dma_chan *chan = &dev->dma_chans[chan_id];
    unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);
    u32 val;

    /* Completion = ControlStatus bits 0|1 both set (legacy ISR semantics),
     * acknowledged by writing 0x03 back. The engine may also signal via
     * IRQ (complete()); accept whichever arrives first. */
    while (time_before(jiffies, end)) {
        if (try_wait_for_completion(&chan->done))
            return 0;

        val = ioread32(chan->regs + C985_DMA_REG_CTRL);
        if ((val & C985_DMA_STATUS_DONE) == C985_DMA_STATUS_DONE) {
            iowrite32(C985_DMA_STATUS_DONE, chan->regs + C985_DMA_REG_CTRL);
            complete(&chan->done);
            return 0;
        }

        usleep_range(50, 200);
    }

    val = ioread32(chan->regs + C985_DMA_REG_CTRL);
    dev_err(&dev->pdev->dev, "DMA[%d] timeout, ControlStatus=0x%08x\n", chan_id, val);
    return -ETIMEDOUT;
}

int c985_dma_read_linear(struct c985_dev *dev, u32 card_addr,
                         dma_addr_t host_phys, u32 len)
{
    const u32 chunk = 0x100000;
    int ret;

    mutex_lock(&dev->dma_read_lock);

    /* Upload path only ever rings the write channel */
    if (!dev->dma_chans[dev->dma_read_chan].desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, 2);
        if (ret)
            goto out;
    }

    while (len) {
        u32 n = min(len, chunk);
        int ret;

        ret = c985_dma_submit(dev, dev->dma_read_chan, host_phys,
                              card_addr, n, false);
        if (ret)
            goto out;
        ret = c985_dma_wait(dev, dev->dma_read_chan, 3000);
        if (ret)
            goto out;

        card_addr += n;
        host_phys += n;
        len -= n;
    }
    ret = 0;
out:
    mutex_unlock(&dev->dma_read_lock);
    return ret;
}

/* Frame-mode (MB2RAS) read: engine converts macroblock-tiled card data to
 * raster in transfer. Geometry per PedDmaQueueBuffers mode 1/2:
 *   OffsetEx = (W/32)<<32 | c<<40 | 3<<56  (c=16 Y / 8 chroma, 3=DataSwap)
 *   card qword = base | OffsetEx | bit59 end-of-chain (single descriptor)
 * No 0x400000000 tag on modes 1/2. */
int c985_dma_read_frame_mode(struct c985_dev *dev, u32 card_addr,
                             dma_addr_t host_phys, u32 len,
                             u32 width, bool chroma)
{
    struct c985_dma_chan *chan = &dev->dma_chans[dev->dma_read_chan];
    struct c985_dma_desc *desc;
    dma_addr_t dpa;
    u32 ctrl = chroma ? 0x4861000F : 0x08BE100F;
    u64 offex;
    int ret;

    mutex_lock(&dev->dma_read_lock);

    if (!chan->in_use) {
        ret = -EINVAL;
        goto out;
    }
    if (!chan->desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, C985_DMA_NUM_DESCS);
        if (ret)
            goto out;
    }
    if (ioread32(chan->regs + C985_DMA_REG_CTRL) & C985_DMA_STATUS_BUSY) {
        ret = -EBUSY;
        goto out;
    }

    if ((ioread32(dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL) & 1) == 0)
        iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);

    reinit_completion(&chan->done);

    /* CardOffsetEx geometry: `width` is the EFFECTIVE plane width (full luma
     * for Y, HALF luma/2 for chroma). byte[32-39] = plane raster stride in
     * 64-bit words = width/32 (Y -> 1920/32=60, chroma -> 960/32=30), so the
     * stride for a chroma plane is half that of luma. byte[40-47] = 16 (Y) /
     * 8 (chroma), byte[56+] = DataSwap 3. */
    offex = ((u64)(width / 32) << 32) |
            ((u64)(chroma ? 8 : 16) << 40) |
            ((u64)3 << 56);

    desc = chan->desc_ring;
    desc->ctrl = cpu_to_le32(ctrl);
    desc->len = cpu_to_le32(len);
    desc->host_addr = cpu_to_le64(host_phys);
    desc->card_addr = cpu_to_le64((u64)card_addr | offex |
                                  0x0800000000000000ULL);
    desc->next_desc = cpu_to_le64(0);
    wmb();

    dpa = (chan->desc_ring_phys + 31) & ~31;
    iowrite32(lower_32_bits(dpa), chan->regs + C985_DMA_REG_DESC_LO);
    iowrite32(upper_32_bits(dpa), chan->regs + C985_DMA_REG_DESC_HI);
    iowrite32(C985_DMA_CTRL_START, chan->regs + C985_DMA_REG_CTRL);

    ret = c985_dma_wait(dev, dev->dma_read_chan, 3000);
out:
    mutex_unlock(&dev->dma_read_lock);
    return ret;
}

/* Internal: build the SG descriptor chain for one frame-mode plane read and
 * kick the engine. Does NOT wait for completion and does NOT take the mutex —
 * the caller holds dev->dma_read_lock. Returns 0 on submit, -EBUSY if engine
 * busy, -EINVAL on short sg_table. Each SG element becomes one chained HW
 * descriptor (Windows PedDmaQueueBuffers model); only the LAST descriptor
 * carries the bit59 end-of-chain marker and next_desc=0.
 */
static int c985_dma_read_frame_mode_sg_kick(struct c985_dev *dev, u32 card_addr,
                                            struct sg_table *sgt, u32 offset,
                                            u32 len, u32 width, bool chroma)
{
    struct c985_dma_chan *chan = &dev->dma_chans[dev->dma_read_chan];
    struct c985_dma_desc *desc;
    struct scatterlist *sg;
    dma_addr_t dpa;
    u32 ctrl = chroma ? 0x4861000F : 0x08BE100F;
    u64 offex;
    u32 card_cur = card_addr, remaining = len, in_elem, offset_orig = offset;
    int ndesc = 0, ret;

    if (!chan->in_use) {
        ret = -EINVAL;
        goto out;
    }
    if (!chan->desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, C985_DMA_NUM_DESCS);
        if (ret)
            goto out;
    }
    if (ioread32(chan->regs + C985_DMA_REG_CTRL) & C985_DMA_STATUS_BUSY) {
        ret = -EBUSY;
        goto out;
    }
    if ((ioread32(dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL) & 1) == 0)
        iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);

    reinit_completion(&chan->done);

    offex = ((u64)(width / 32) << 32) |
            ((u64)(chroma ? 8 : 16) << 40) |
            ((u64)3 << 56);

    /* Advance to the SG element containing the plane's starting offset. */
    sg = sgt->sgl;
    while (sg_dma_len(sg) > 0 && offset >= (u32)sg_dma_len(sg)) {
        offset -= (u32)sg_dma_len(sg);
        sg = sg_next(sg);
        if (!sg) {
            ret = -EINVAL;
            goto out;
        }
    }
    in_elem = offset;

    desc = chan->desc_ring;
    dpa = (chan->desc_ring_phys + 31) & ~31;

    while (remaining && sg) {
        u32 seglen = (u32)sg_dma_len(sg) - in_elem;
        u32 n = min(seglen, remaining);
        bool last;

        if (ndesc >= C985_DMA_NUM_DESCS) {
            ret = -EINVAL;
            goto out;
        }

        last = (n == remaining);

        desc[ndesc].ctrl = cpu_to_le32(ctrl);
        desc[ndesc].len = cpu_to_le32(n);
        desc[ndesc].host_addr = cpu_to_le64(sg_dma_address(sg) + in_elem);
        desc[ndesc].card_addr = cpu_to_le64((u64)card_cur | offex |
                                            (last ? 0x0800000000000000ULL : 0));
        desc[ndesc].next_desc = cpu_to_le64(last ? 0 :
                                             dpa + (ndesc + 1) * sizeof(*desc));

        card_cur += n;
        remaining -= n;
        in_elem = 0;
        ndesc++;
        sg = sg_next(sg);
    }

    if (remaining) {
        ret = -EINVAL;
        goto out;
    }

    wmb();
    iowrite32(lower_32_bits(dpa), chan->regs + C985_DMA_REG_DESC_LO);
    iowrite32(upper_32_bits(dpa), chan->regs + C985_DMA_REG_DESC_HI);
    iowrite32(C985_DMA_CTRL_START, chan->regs + C985_DMA_REG_CTRL);
    return 0;

out:
    dev_dbg(&dev->pdev->dev,
            "DMA SG submit fail: card=0x%x off=%u len=%u ndesc=%d ret=%d\n",
            card_addr, offset_orig, len, ndesc, ret);
    return ret;
}

/* Synchronous frame-mode SG read (one plane). Blocks for DMA completion, used
 * by the linear legacy paths that still read planes inline. */
int c985_dma_read_frame_mode_sg(struct c985_dev *dev, u32 card_addr,
                                struct sg_table *sgt, u32 offset, u32 len,
                                u32 width, bool chroma)
{
    int ret;

    if (!sgt || !sgt->sgl)
        return -EINVAL;

    mutex_lock(&dev->dma_read_lock);
    ret = c985_dma_read_frame_mode_sg_kick(dev, card_addr, sgt, offset,
                                           len, width, chroma);
    if (ret == 0)
        ret = c985_dma_wait(dev, dev->dma_read_chan, 3000);
    mutex_unlock(&dev->dma_read_lock);
    return ret;
}

/* Asynchronous frame-mode SG read: submit ONE plane, return immediately.
 * Completion is signalled via the completion counter; the frame-op state
 * machine advances on the ISR-scheduled continuation work. Caller must hold
 * dev->dma_read_lock (the frame-op path takes it around the full Y/U/V seq). */
int c985_dma_read_frame_mode_sg_submit(struct c985_dev *dev, u32 card_addr,
                                       struct sg_table *sgt, u32 offset,
                                       u32 len, u32 width, bool chroma)
{
    return c985_dma_read_frame_mode_sg_kick(dev, card_addr, sgt, offset,
                                            len, width, chroma);
}

/* ---- Async full-frame DMA (Y -> U -> V serial, Windows DPC model) ----
 *
 * c985_dma_submit_frame() starts the Y plane. The ISR, on DMA completion for
 * the read channel, schedules c985_dma_frame_work, which calls
 * c985_dma_frame_next() to advance U -> V -> completion. done_cb owns buffer
 * handoff + 0x30 release. Plane card addresses (DWORD -> byte, dw<<2):
 *   Y = desc.y_dw << 2; U = desc.u_dw << 2; V = U + C985_V_OFFSET_BYTES.
 */

static void c985_dma_frame_work(struct work_struct *w)
{
    struct c985_frame_op *op = container_of(w, struct c985_frame_op, work);
    c985_dma_frame_next(op);
}

/* Advance a frame op to its next plane, or complete it. Runs in process
 * context (workqueue) because plane submission may lazily alloc the desc ring
 * via dma_alloc_coherent (not hard-IRQ safe). */
void c985_dma_frame_next(struct c985_frame_op *op)
{
    struct c985_dev *dev = op->dev;
    u32 card = 0, offset = 0, len = 0;
    bool chroma;
    int ret;
    unsigned long flags;

    if (op->failed)
        goto complete;

    switch (op->phase) {
    case 0: /* Y done -> U */
        card = op->desc.u_dw << 2;
        offset = C985_Y_LEN;
        len = C985_C_LEN;
        chroma = true;
        op->phase = 1;
        break;
    case 1: /* U done -> V */
        card = (op->desc.u_dw << 2) + C985_V_OFFSET_BYTES;
        offset = C985_Y_LEN + C985_C_LEN;
        len = C985_C_LEN;
        chroma = true;
        op->phase = 2;
        break;
    default:
        goto complete;
    }

    mutex_lock(&dev->dma_read_lock);
    ret = c985_dma_read_frame_mode_sg_submit(dev, card, op->sgt, offset,
                                             len, C985_WIDTH / 2, chroma);
    mutex_unlock(&dev->dma_read_lock);
    if (ret) {
        dev_err(&dev->pdev->dev,
                "frame op: plane %d submit failed: %d\n", op->phase, ret);
        op->failed = true;
        goto complete;
    }
    /* op remains dev->dma_cur_op for the ISR to schedule the next plane. */
    return;

complete:
    spin_lock_irqsave(&dev->dma_cur_op_lock, flags);
    dev->dma_cur_op = NULL;
    spin_unlock_irqrestore(&dev->dma_cur_op_lock, flags);
    op->phase = 3;
    if (op->done_cb)
        op->done_cb(op);
}

/* Submit a full frame: start Y; U/V driven by DMA completion ISR. Returns
 * -EBUSY if another frame op is still in flight (single read engine + single
 * dma_cur_op: the caller must retry/drop). */
int c985_dma_submit_frame(struct c985_frame_op *op)
{
    struct c985_dev *dev = op->dev;
    unsigned long flags;
    int ret;

    op->phase = 0;
    op->failed = false;
    INIT_WORK(&op->work, c985_dma_frame_work);

    /* Claim the single in-flight slot before kicking Y, so a concurrent
     * frame_consumer cannot overwrite dma_cur_op mid-sequence. */
    spin_lock_irqsave(&dev->dma_cur_op_lock, flags);
    if (dev->dma_cur_op) {
        spin_unlock_irqrestore(&dev->dma_cur_op_lock, flags);
        return -EBUSY;
    }
    dev->dma_cur_op = op;
    spin_unlock_irqrestore(&dev->dma_cur_op_lock, flags);

    mutex_lock(&dev->dma_read_lock);
    ret = c985_dma_read_frame_mode_sg_submit(dev, op->desc.y_dw << 2,
                                             op->sgt, 0, C985_Y_LEN,
                                             C985_WIDTH, false);
    mutex_unlock(&dev->dma_read_lock);
    if (ret) {
        /* Failed to kick Y: release the in-flight slot so the next frame
         * can be attempted. */
        spin_lock_irqsave(&dev->dma_cur_op_lock, flags);
        dev->dma_cur_op = NULL;
        spin_unlock_irqrestore(&dev->dma_cur_op_lock, flags);
        return ret;
    }

    return 0;
}