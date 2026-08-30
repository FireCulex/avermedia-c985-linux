// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/* CPR (Card Program Register) primitives for kernel driver
 * Transcribed from Windows driver assembly (CPR_MemoryRead/Write)
 *
 */

#define CPR_TIMEOUT_MS        3000
#define CPR_STATUS_MASK16     0xFFFF0003u
#define CPR_WR_START          0x04u
#define CPR_RD_START          0x10u

/* Poll mode: 0 = branch A (chipver==0x10), 1 = branch B (chipver!=0x10) */
static int cpr_poll_mode = 1;
static u32 cpr_mem_size_units = 0x200u;  /* read clamp, in 0x40000 units */

static inline u32 cpr_read_bar1_reg(struct c985_dev *dev, u32 offset)
{
    return ioread32(dev->bar1 + offset);
}

static inline void cpr_write_bar1_reg(struct c985_dev *dev, u32 offset, u32 val)
{
    iowrite32(val, dev->bar1 + offset);
}

static inline void rw_barrier(void)
{
    /* Memory barrier for MMIO ordering */
    wmb();
}

static int cpr_wr_done(struct c985_dev *dev, u32 *status)
{
    unsigned t;
    for (t = 0; t < CPR_TIMEOUT_MS; t++) {
        u32 v = cpr_read_bar1_reg(dev, C985_CPR_WR_CTL);
        *status = v;
        if (cpr_poll_mode == 0) {  /* branch A: chipver==0x10 */
            if (((v >> 18) & 0x3F) == 0)
                return 0;
        } else {  /* branch B */
            if (((v >> 18) & 0xFF) == 0x42)
                return 0;
        }
        msleep(1);
    }
    return -ETIMEDOUT;
}

static int cpr_rd_done(struct c985_dev *dev, u32 *status)
{
    unsigned t;
    for (t = 0; t < CPR_TIMEOUT_MS; t++) {
        u32 v = cpr_read_bar1_reg(dev, C985_CPR_RD_CTL);
        *status = v;
        if (cpr_poll_mode == 0) {  /* branch A */
            u32 s = (v >> 18) & 0x3F;
            if (s != 0x3F && s != 0)
                return 0;
        } else {  /* branch B: loop while ==0 */
            if (((v >> 18) & 0x3F) != 0)
                return 0;
        }
        msleep(1);
    }
    return -ETIMEDOUT;
}

int c985_cpr_write(struct c985_dev *dev, u32 addr, u32 data, u32 *status)
{
    u32 a, ctl;

    a = ((addr >> 2) & 0x7FFFFFFu) << 2;
    cpr_write_bar1_reg(dev, C985_CPR_WR_ADDR, a);
    rw_barrier();

    ctl = cpr_read_bar1_reg(dev, C985_CPR_WR_CTL);
    ctl = (ctl & CPR_STATUS_MASK16) | CPR_WR_START;
    cpr_write_bar1_reg(dev, C985_CPR_WR_CTL, ctl);
    rw_barrier();

    cpr_write_bar1_reg(dev, C985_CPR_WR_DATA, data);
    rw_barrier();

    return cpr_wr_done(dev, status);
}

int c985_cpr_read(struct c985_dev *dev, u32 addr, u32 *data, u32 *status)
{
    u32 a, ctl, arr[4];
    u32 mem_bytes = cpr_mem_size_units * 0x40000u;
    u32 word = addr >> 2;
    u32 idx = 0;
    int i;

    /* Driver clamp: if (word + 4) > memBytes then word = memBytes - 4 */
    if (word + 4 > mem_bytes) {
        idx = (word + 4) - mem_bytes;
        word = mem_bytes - 4;
    }

    a = (word & 0x7FFFFFFu) << 2;
    cpr_write_bar1_reg(dev, C985_CPR_RD_ADDR, a);
    rw_barrier();

    ctl = cpr_read_bar1_reg(dev, C985_CPR_RD_CTL);
    ctl = (ctl & CPR_STATUS_MASK16) | CPR_RD_START;
    cpr_write_bar1_reg(dev, C985_CPR_RD_CTL, ctl);
    rw_barrier();

    if (cpr_rd_done(dev, status) != 0)
        return -ETIMEDOUT;

    for (i = 0; i < 4; i++)
        arr[i] = cpr_read_bar1_reg(dev, C985_CPR_RD_DATA);
    *data = arr[idx & 3];
    return 0;
}