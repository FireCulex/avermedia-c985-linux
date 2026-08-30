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
    dev_dbg(&dev->pdev->dev, "DMA[%d]: allocated %d descs at 0x%pad\n",
             chan_id, num_descs, &chan->desc_ring_phys);
    return 0;
}

void c985_dma_free_desc(struct c985_dev *dev, int chan_id)
{
    struct c985_dma_chan *chan = &dev->dma_chans[chan_id];
    if (chan->desc_ring) {
        dma_free_coherent(&dev->pdev->dev,
                          2 * sizeof(struct c985_dma_desc) + C985_DMA_DESC_ALIGN,
                          chan->desc_ring, chan->desc_ring_phys);
        chan->desc_ring = NULL;
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

    /* Upload path only ever rings the write channel */
    if (!dev->dma_chans[dev->dma_read_chan].desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, 2);
        if (ret)
            return ret;
    }

    while (len) {
        u32 n = min(len, chunk);
        int ret;

        ret = c985_dma_submit(dev, dev->dma_read_chan, host_phys,
                              card_addr, n, false);
        if (ret)
            return ret;
        ret = c985_dma_wait(dev, dev->dma_read_chan, 3000);
        if (ret)
            return ret;

        card_addr += n;
        host_phys += n;
        len -= n;
    }
    return 0;
}

/* --- Async frame-mode DMA: submit one plane WITHOUT waiting ---
 * Builds a single-descriptor frame-mode read and kicks the C2S engine.
 * Completion is signalled via the completion counter; the caller (frame op
 * state machine) advances to the next plane from the ISR-scheduled work.
 * Returns 0 submitted, -EBUSY if engine already busy, -EINVAL on bad state. */
int c985_dma_read_frame_mode_submit(struct c985_dev *dev, u32 card_addr,
                                    dma_addr_t host_phys, u32 len,
                                    u32 width, bool chroma)
{
    struct c985_dma_chan *chan = &dev->dma_chans[dev->dma_read_chan];
    struct c985_dma_desc *desc;
    dma_addr_t dpa;
    u32 ctrl = chroma ? C985_DMA_CTRL_FRAME_UV : C985_DMA_CTRL_FRAME_Y;
    u64 offex;
    int ret;

    if (!chan->in_use)
        return -EINVAL;
    if (!chan->desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, 2);
        if (ret)
            return ret;
    }
    if (ioread32(chan->regs + C985_DMA_REG_CTRL) & C985_DMA_STATUS_BUSY)
        return -EBUSY;

    if ((ioread32(dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL) & 1) == 0)
        iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);

    reinit_completion(&chan->done);

    /* CardOffsetEx geometry IDENTICAL to the verified c985_dma_read_frame_mode
     * path: byte[32-39] = width/32,
     * byte[40-47] = 16 (Y) / 8 (chroma), byte[56+] = DataSwap 3. */
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

    return 0;
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

    if (!chan->in_use)
        return -EINVAL;
    if (!chan->desc_ring) {
        ret = c985_dma_alloc_desc(dev, dev->dma_read_chan, 2);
        if (ret)
            return ret;
    }
    if (ioread32(chan->regs + C985_DMA_REG_CTRL) & C985_DMA_STATUS_BUSY)
        return -EBUSY;

    if ((ioread32(dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL) & 1) == 0)
        iowrite32(1, dev->bar0 + C985_DMA_GLOBAL_BASE + C985_DMA_GLOBAL_CTRL);

    reinit_completion(&chan->done);

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

    return c985_dma_wait(dev, dev->dma_read_chan, 3000);
}

/* --- Async full-frame DMA (Y -> U -> V serial, Windows model) ---
 *
 * c985_dma_submit_frame() starts the Y plane. The ISR, on DMA completion
 * for the read channel, calls c985_dma_frame_next() (below) which advances
 * through U then V and finally fires op->done_cb with all planes complete.
 *
 * Plane card addresses (DWORD -> byte, per descriptor CardAddr = dw<<2):
 *   Y = desc.y_dw << 2
 *   U = desc.u_dw << 2
 *   V = U + C985_V_OFFSET_BYTES (0x40)  [U+0x10 DWORDs, asm-verified]
 * }
 */

/* Advance a frame op to its next plane, or complete it. Runs in process
 * context (workqueue) because plane submission may lazily allocate a
 * descriptor ring via dma_alloc_coherent (not hard-IRQ safe). */
void c985_dma_frame_next(struct c985_frame_op *op)
{
    struct c985_dev *dev = op->dev;
    u32 phys = 0, card = 0, len = 0;
    bool chroma;
    int ret;

    if (op->failed)
        goto complete;

    switch (op->phase) {
    case 0: /* Y done -> U */
        phys = op->u_phys;
        card = op->desc.u_dw << 2;
        len = op->c_len;
        chroma = true;
        op->phase = 1;
        break;
    case 1: /* U done -> V */
        phys = op->v_phys;
        card = (op->desc.u_dw << 2) + C985_V_OFFSET_BYTES;
        len = op->c_len;
        chroma = true;
        op->phase = 2;
        break;
    default:
        goto complete;
    }

    ret = c985_dma_read_frame_mode_submit(dev, card, phys, len,
                                          op->width, chroma);
    if (ret) {
        dev_err(&dev->pdev->dev,
                "frame op: plane %d submit failed: %d\n", op->phase, ret);
        op->failed = true;
        goto complete;
    }
    dev->dma_cur_op = op;
    return;

complete:
    dev->dma_cur_op = NULL;
    if (op->done_cb)
        op->done_cb(op);
}

static void c985_dma_frame_work(struct work_struct *w)
{
    struct c985_frame_op *op = container_of(w, struct c985_frame_op, work);
    c985_dma_frame_next(op);
}

/* Submit a full frame. Starts the Y plane; subsequent planes are driven by
 * DMA completion (ISR schedules c985_dma_frame_work) via frame_next(). */
int c985_dma_submit_frame(struct c985_frame_op *op)
{
    struct c985_dev *dev = op->dev;
    int ret;

    op->phase = 0;
    op->failed = false;
    INIT_WORK(&op->work, c985_dma_frame_work);

    ret = c985_dma_read_frame_mode_submit(dev, op->desc.y_dw << 2,
                                          op->y_phys, op->y_len,
                                          op->width, false);
    if (ret)
        return ret;

    dev->dma_cur_op = op;
    return 0;
}