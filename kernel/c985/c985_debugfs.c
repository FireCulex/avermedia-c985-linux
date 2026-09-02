// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/* ---- Debugfs ---- */

static int c985_debugfs_regs_show(struct seq_file *m, void *v)
{
    struct c985_dev *dev = m->private;

    seq_printf(m, "=== BAR0 (DMA/PCIe) ===\n");
    seq_printf(m, "0x4000 DMA_COMMON_CTL: 0x%08x\n", c985_read_bar0(dev, 0x4000));
    seq_printf(m, "0x8030 PCIE_ARM_IRQ:   0x%08x\n", c985_read_bar0(dev, 0x8030));

    seq_printf(m, "\n=== BAR1 (Main/Mailbox/ARM) ===\n");
    seq_printf(m, "0x000 CHIP_CONTROL:     0x%08x\n", c985_read_bar1(dev, 0x000));
    seq_printf(m, "0x004 IRQ_STATUS:       0x%08x\n", c985_read_bar1(dev, 0x004));
    seq_printf(m, "0x00C IRQ_MASK:         0x%08x\n", c985_read_bar1(dev, 0x00C));
    seq_printf(m, "0x024 DOORBELL:         0x%08x\n", c985_read_bar1(dev, 0x024));
    seq_printf(m, "0x030 STATUS:           0x%08x\n", c985_read_bar1(dev, 0x030));
    seq_printf(m, "0x038 CHIP_VER:         0x%08x\n", c985_read_bar1(dev, 0x038));
    seq_printf(m, "0x010 TIMER_CTL:        0x%08x\n", c985_read_bar1(dev, 0x010));
    seq_printf(m, "0x014 TIMER_CMP:        0x%08x\n", c985_read_bar1(dev, 0x014));
    seq_printf(m, "0x01C TIMER_CNT:        0x%08x\n", c985_read_bar1(dev, 0x01C));
    seq_printf(m, "0x600 MAILBOX_CTRL:     0x%08x\n", c985_read_bar1(dev, 0x600));
    seq_printf(m, "0x610 SIGNAL_STATUS:    0x%08x\n", c985_read_bar1(dev, 0x610));
    seq_printf(m, "0x618 SIGNAL_STATUS2:   0x%08x\n", c985_read_bar1(dev, 0x618));
    seq_printf(m, "0x6C8 FROM_ARM_STATUS:  0x%08x\n", c985_read_bar1(dev, 0x6C8));
    seq_printf(m, "0x6CC TO_ARM_STATUS:    0x%08x\n", c985_read_bar1(dev, 0x6CC));
    seq_printf(m, "0x6FC TO_ARM_MSG:       0x%08x\n", c985_read_bar1(dev, 0x6FC));
    seq_printf(m, "0x6B0 FROM_ARM_MSG:     0x%08x\n", c985_read_bar1(dev, 0x6B0));
    seq_printf(m, "0x800 HCI_INT_MASK:     0x%08x\n", c985_read_bar1(dev, 0x800));
    seq_printf(m, "0x804 HCI_INT_STATUS:   0x%08x\n", c985_read_bar1(dev, 0x804));
    seq_printf(m, "0x80C ARM_BOOT:         0x%08x\n", c985_read_bar1(dev, 0x80C));
    seq_printf(m, "0x840 MEM_CTL:          0x%08x\n", c985_read_bar1(dev, 0x840));
    seq_printf(m, "0x4000 PCIE_IRQ_CTRL:   0x%08x\n", c985_read_bar1(dev, 0x4000));
    seq_printf(m, "0xE04 DOORBELL_E04:     0x%08x\n", c985_read_bar1(dev, 0xE04));

    seq_printf(m, "\n=== IRQ Stats ===\n");
    seq_printf(m, "Total:           %d\n", atomic_read(&dev->irq_count));
    seq_printf(m, "PCIe ARM:        %d\n", atomic_read(&dev->irq_pcie_count));
    seq_printf(m, "HCI:             %d\n", atomic_read(&dev->irq_hci_count));
    seq_printf(m, "Doorbell:        %d\n", atomic_read(&dev->irq_doorbell_count));
    seq_printf(m, "Mailbox HIU:     %d\n", atomic_read(&dev->irq_mbox_hiu_count));
    seq_printf(m, "Mailbox SW1:     %d\n", atomic_read(&dev->irq_mbox_sw1_count));
    seq_printf(m, "BAR1+0x700:      %d\n", atomic_read(&dev->irq_bar1_700_count));
    seq_printf(m, "BAR1+0xE04:      %d\n", atomic_read(&dev->irq_bar1_e04_count));
    seq_printf(m, "DMA:             %d\n", atomic_read(&dev->irq_dma_count));

    return 0;
}

static int c985_debugfs_regs_open(struct inode *inode, struct file *file)
{
    return single_open(file, c985_debugfs_regs_show, inode->i_private);
}

static const struct file_operations c985_debugfs_regs_fops = {
    .owner = THIS_MODULE,
    .open = c985_debugfs_regs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static ssize_t c985_debugfs_write_reg(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[64];
    unsigned long bar = 0, addr = 0, val = 0;
    char *p, *endp = NULL;
    int ret;

    if (count >= sizeof(kbuf))
        return -EINVAL;

    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    p = kbuf;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return -EINVAL;

    endp = p;
    while (*endp && *endp != ' ' && *endp != '\t' && *endp != '\n') endp++;
    *endp = '\0';

    if (strcmp(p, "bar0") == 0) bar = 0;
    else if (strcmp(p, "bar1") == 0) bar = 1;
    else return -EINVAL;

    p = endp + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return -EINVAL;

    endp = p;
    while (*endp && *endp != ' ' && *endp != '\t' && *endp != '\n') endp++;
    *endp = '\0';

    ret = kstrtoul(p, 0, &addr);
    if (ret < 0) return -EINVAL;

    p = endp + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return -EINVAL;

    ret = kstrtoul(p, 0, &val);
    if (ret < 0) return -EINVAL;

    if (bar == 0)
        c985_write_bar0(dev, addr, val);
    else
        c985_write_bar1(dev, addr, val);

    return count;
}

static const struct file_operations c985_debugfs_write_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_write_reg,
    .open = simple_open,
};

/* ---- Register readback ----
 * Usage:  echo "bar0|bar1 <addr>" > read_reg
 *         cat read_reg             -> "0x%08x"
 * Needed for QL201 I2C-engine access (BAR1+0x500/0x504/0x50C, NUC100).
 */

static unsigned long rd_bar, rd_addr;

static ssize_t c985_debugfs_read_reg_write(struct file *file,
                                            const char __user *buf,
                                            size_t count, loff_t *ppos)
{
    char kbuf[64];
    unsigned long bar = 0, addr = 0;
    char *p, *endp = NULL;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = '\0';

    p = kbuf;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return -EINVAL;

    endp = p;
    while (*endp && *endp != ' ' && *endp != '\t' && *endp != '\n') endp++;
    *endp = '\0';
    if (strcmp(p, "bar0") == 0) bar = 0;
    else if (strcmp(p, "bar1") == 0) bar = 1;
    else return -EINVAL;

    p = endp + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (!*p) return -EINVAL;

    if (kstrtoul(p, 0, &addr) < 0 || addr > 0xFFFF)
        return -EINVAL;

    rd_bar = bar;
    rd_addr = addr;
    return count;
}

static ssize_t c985_debugfs_read_reg_read(struct file *file, char __user *buf,
                                           size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char tmp[16];
    int len;
    u32 v;

    v = rd_bar ? c985_read_bar1(dev, rd_addr) : c985_read_bar0(dev, rd_addr);

    len = snprintf(tmp, sizeof(tmp), "0x%08x\n", v);
    return simple_read_from_buffer(buf, count, ppos, tmp, len);
}

static const struct file_operations c985_debugfs_read_reg_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_read_reg_write,
    .read = c985_debugfs_read_reg_read,
    .open = simple_open,
};

/* ---- Mailbox test trigger ----
 * Usage:  echo "opcode param0 [taskid] [timeout_ms] [hasresp] [slot0..slot9]"
 *           > mbox_send
 *   opcode/param0 any base; taskid default 8, timeout default 1000 ms,
 *   hasresp default 0.  slot0..slot9 fill the payload window 0x6F4..0x6D0
 *   (encoder config block consumed by the down-walk on 0x01/0x06/etc).
 */

static int c985_debugfs_parse_tokens(char *kbuf, unsigned long *toks,
                                      int max_toks, int *ntoks)
{
    char *p = kbuf;
    int n = 0;

    while (n < max_toks) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p)
            break;
        {
            char *endp = p;
            while (*endp && *endp != ' ' && *endp != '\t' && *endp != '\n')
                endp++;
            if (*endp)
                *endp++ = '\0';
            if (kstrtoul(p, 0, &toks[n]) < 0)
                return -EINVAL;
            n++;
            p = endp;
        }
    }
    *ntoks = n;
    pr_debug("parse_tokens: ntoks=%d, tokens: %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx %lx\n",
             n, toks[0], toks[1], toks[2], toks[3], toks[4], toks[5], toks[6], toks[7], toks[8], toks[9], toks[10], toks[11], toks[12], toks[13], toks[14]);
    return 0;
}

static ssize_t c985_debugfs_mbox_send(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[256];
    unsigned long toks[15];
    int ntoks, i;
    u16 opcode;
    u32 param0;
    u8 task_id = 8;
    unsigned long timeout_ms = 1000;
    bool has_resp = false;
    int ret;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    memset(kbuf, 0, sizeof(kbuf));
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    if (c985_debugfs_parse_tokens(kbuf, toks, ARRAY_SIZE(toks), &ntoks) < 0 ||
        ntoks < 1)
        return -EINVAL;

    if (toks[0] > 0xFFFF)
        return -EINVAL;
    opcode = (u16)toks[0];
    param0 = ntoks > 1 ? (u32)toks[1] : 0;
    if (ntoks > 2)
        task_id = (u8)(toks[2] & 0xFF);
    if (ntoks > 3 && toks[3] > 0)
        timeout_ms = toks[3];
    if (ntoks > 4)
        has_resp = toks[4] != 0;

    dev_dbg(&dev->pdev->dev, "mbox_send debugfs: ntoks=%d, opcode=0x%02x param0=0x%08x task=%u timeout=%lu has_resp=%d\n",
             ntoks, opcode, param0, task_id, timeout_ms, has_resp);

    /* toks[5..14] -> payload window 0x6F4..0x6D0 */
    for (i = 0; i < ARRAY_SIZE(dev->mbox_slots); i++)
        dev->mbox_slots[i] = ntoks > 5 + i ? (u32)toks[5 + i] : 0;

    dev_dbg(&dev->pdev->dev, "mbox_send debugfs: mbox_slots[0..4]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
             dev->mbox_slots[0], dev->mbox_slots[1], dev->mbox_slots[2], dev->mbox_slots[3], dev->mbox_slots[4]);

ret = c985_mbox_send_polling(dev, opcode, param0, task_id, has_resp, false,
                             timeout_ms);

    dev_dbg(&dev->pdev->dev,
             "mbox_send debugfs: ret=%d clear_us=%lld resp_us=%lld ack=0x%08x\n",
             ret, dev->last_mbox.clear_us, dev->last_mbox.resp_us,
             dev->last_mbox.ack_word);
    return ret ? ret : count;
}

static const struct file_operations c985_debugfs_mbox_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_mbox_send,
    .open = simple_open,
};

static int c985_debugfs_mbox_status_show(struct seq_file *m, void *v)
{
    struct c985_dev *dev = m->private;
    struct c985_mbox_result *r = &dev->last_mbox;

    seq_printf(m, "have_result:      %d\n", dev->have_mbox_result);
    seq_printf(m, "status_sent:      0x%08x\n", r->status_sent);
    seq_printf(m, "msg_written:      0x%08x\n", r->msg_written);
    seq_printf(m, "param0_written:   0x%08x\n", r->param0_written);
    seq_printf(m, "clear_us:         %lld%s\n",
               r->clear_us, r->clear_us < 0 ? " (TIMEOUT)" : "");
    seq_printf(m, "resp_us:          %lld%s\n",
               r->resp_us, r->resp_us < 0 ? " (none)" : "");
    seq_printf(m, "resp_ok:          %d\n", r->resp_ok);
    seq_printf(m, "from_arm_msg:     0x%08x\n", r->from_arm_msg);
    seq_printf(m, "from_arm_status:  0x%08x\n", r->from_arm_status);
    seq_printf(m, "resp_params:      %08x %08x %08x %08x %08x\n",
               r->resp_params[0], r->resp_params[1], r->resp_params[2],
               r->resp_params[3], r->resp_params[4]);
    seq_printf(m, "ack_word:         0x%08x\n", r->ack_word);
    if (dev->have_cpr_peek)
        seq_printf(m, "cpr_peek[0x%06x]: 0x%08x\n",
                   dev->cpr_peek_addr, dev->cpr_peek_val);
    seq_printf(m, "\ncurrent live regs:\n");
    seq_printf(m, "0x6CC TO_ARM_STATUS:  0x%08x\n",
               c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS));
    seq_printf(m, "0x6FC TO_ARM_MSG:     0x%08x\n",
               c985_read_bar1(dev, C985_TO_ARM_MESSAGE));
    seq_printf(m, "0x6B0 FROM_ARM_MSG:   0x%08x\n",
               c985_read_bar1(dev, C985_FROM_ARM_MESSAGE));
    seq_printf(m, "0x6C8 FROM_ARM_STATUS: 0x%08x\n",
               c985_read_bar1(dev, C985_FROM_ARM_MSG_STATUS));
    seq_printf(m, "resp_params:      %08x %08x %08x %08x %08x\n",
               c985_read_bar1(dev, 0x6B4),
               c985_read_bar1(dev, 0x6B8),
               c985_read_bar1(dev, 0x6BC),
               c985_read_bar1(dev, 0x6C0),
               c985_read_bar1(dev, 0x6C4));
    return 0;
}

static int c985_debugfs_mbox_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, c985_debugfs_mbox_status_show, inode->i_private);
}

static const struct file_operations c985_debugfs_mbox_status_fops = {
    .owner = THIS_MODULE,
    .open = c985_debugfs_mbox_status_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

/* ---- NUC100 HDMI signal status (GPIO bit-bang I2C) ---- */
static int c985_debugfs_nuc_show(struct seq_file *m, void *v)
{
	struct c985_dev *dev = m->private;
	u8 id3[3], status, raw7[7];
	u16 hact, vact, htot, vtot;
	int pclk, ret;

	ret = c985_nuc100_status(dev, id3, &status, &hact, &vact,
				 &htot, &vtot, &pclk, raw7);
	if (ret) {
		seq_printf(m, "error=%d\n", ret);
		return 0;
	}

	seq_printf(m, "id=%c%c%c present=%d status=0x%02x",
		   id3[0] >= 0x20 && id3[0] < 0x7f ? id3[0] : '?',
		   id3[1] >= 0x20 && id3[1] < 0x7f ? id3[1] : '?',
		   id3[2] >= 0x20 && id3[2] < 0x7f ? id3[2] : '?',
		   (status & 0x04) ? 1 : 0, status);
	if (status & 0x04)
		seq_printf(m, " %ux%u total %ux%u pclk=%dkHz",
			   hact, vact, htot, vtot, pclk);
	seq_printf(m, " raw7=%02x %02x %02x %02x %02x %02x %02x\n",
		   raw7[0], raw7[1], raw7[2], raw7[3], raw7[4], raw7[5],
		   raw7[6]);
	return 0;
}

static int c985_debugfs_nuc_open(struct inode *inode, struct file *file)
{
	return single_open(file, c985_debugfs_nuc_show, inode->i_private);
}

static const struct file_operations c985_debugfs_nuc_fops = {
	.owner = THIS_MODULE,
	.open = c985_debugfs_nuc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ---- CPR peek: echo addr > cpr_peek, then read mailbox_status ---- */
static ssize_t c985_debugfs_cpr_peek(struct file *file, const char __user *buf,
                                     size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[32];
    unsigned long addr;
    u32 val = 0, status;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    memset(kbuf, 0, sizeof(kbuf));
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    if (kstrtoul(kbuf, 0, &addr) < 0 || addr > 0xFFFFFFF)
        return -EINVAL;

    if (c985_cpr_read(dev, (u32)addr, &val, &status)) {
        dev_err(&dev->pdev->dev, "cpr_peek 0x%08lx failed (status=0x%08x)\n",
                addr, status);
        return -EIO;
    }

    dev->cpr_peek_addr = (u32)addr;
    dev->cpr_peek_val = val;
    dev->have_cpr_peek = true;
    dev_dbg(&dev->pdev->dev, "cpr_peek[0x%08lx] = 0x%08x\n", addr, val);
    return count;
}

static const struct file_operations c985_debugfs_cpr_peek_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_cpr_peek,
    .open = simple_open,
};

/* ---- Frame readback ----
 * Usage:  echo "card_addr len" > frame_read   (hex ok; len<=8MB, 4-aligned)
 *         cat frame_read                      -> raw bytes from card DDR
 * DMA-reads card memory via the C2S engine (linear read descriptors).
 */

static ssize_t c985_debugfs_frame_write(struct file *file,
                                        const char __user *buf,
                                        size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[64];
    unsigned long toks[2];
    int ntoks;
    unsigned long addr, len = C985_FRAME_MAX;
    int ret;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    memset(kbuf, 0, sizeof(kbuf));
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    if (c985_debugfs_parse_tokens(kbuf, toks, ARRAY_SIZE(toks), &ntoks) < 0 ||
        ntoks < 1)
        return -EINVAL;

    addr = toks[0];
    if (ntoks > 1)
        len = toks[1];

    if (addr > 0xFFFFFFFFull || len == 0 || len > C985_FRAME_MAX)
        return -EINVAL;
    /* Ring frame addresses/lengths are byte-granular; round len only */
    len = (len + 3u) & ~3ul;

    mutex_lock(&dev->frame_lock);
    if (!dev->frame_buf || dev->frame_buf_size < len) {
        if (dev->frame_buf)
            dma_free_coherent(&dev->pdev->dev, dev->frame_buf_size,
                              dev->frame_buf, dev->frame_buf_phys);
        dev->frame_buf = NULL;
        dev->frame_buf_size = 0;

        dev->frame_buf = dma_alloc_coherent(&dev->pdev->dev, len,
                                            &dev->frame_buf_phys,
                                            GFP_KERNEL);
        if (!dev->frame_buf) {
            mutex_unlock(&dev->frame_lock);
            return -ENOMEM;
        }
        dev->frame_buf_size = len;
    }

    ret = c985_dma_read_linear(dev, (u32)addr, dev->frame_buf_phys,
                               (u32)len);
	if (!ret) {
		dev->frame_len = len;
		dev->frame_valid = true;
		dev_dbg(&dev->pdev->dev,
			"frame_read: card=0x%08lx len=%lu -> host phys 0x%pad\n",
			addr, len, &dev->frame_buf_phys);
	} else {
        dev_err(&dev->pdev->dev,
                "frame_read: card=0x%08lx len=%lu failed: %d\n",
                addr, len, ret);
    }
    mutex_unlock(&dev->frame_lock);

    return ret ? ret : count;
}

static ssize_t c985_debugfs_frame_read(struct file *file, char __user *buf,
                                       size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    ssize_t ret;

    mutex_lock(&dev->frame_lock);
    if (!dev->frame_valid) {
        mutex_unlock(&dev->frame_lock);
        return -ENODATA;
    }
    ret = simple_read_from_buffer(buf, count, ppos, dev->frame_buf,
                                  dev->frame_len);
    mutex_unlock(&dev->frame_lock);
    return ret;
}

static const struct file_operations c985_debugfs_frame_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_frame_write,
    .read = c985_debugfs_frame_read,
    .open = simple_open,
};

/* ---- Frame-mode (MB2RAS) readback ----
 * Usage: echo "card_addr len width chroma" > frame_mb   (chroma: 0=Y 1=UV)
 *        cat frame_mb -> rasterized plane bytes
 */
static ssize_t c985_debugfs_frame_mb_write(struct file *file,
                                           const char __user *buf,
                                           size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[64];
    unsigned long toks[4];
    int ntoks, ret;
    unsigned long addr, len, width, chroma = 0;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    memset(kbuf, 0, sizeof(kbuf));
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    if (c985_debugfs_parse_tokens(kbuf, toks, ARRAY_SIZE(toks), &ntoks) < 0 ||
        ntoks < 3)
        return -EINVAL;

    addr = toks[0];
    len = toks[1];
    width = toks[2];
    if (ntoks > 3)
        chroma = toks[3];

    if (addr > 0xFFFFFFFFull || len == 0 || len > C985_FRAME_MAX ||
        width < 32 || width % 32)
        return -EINVAL;

    mutex_lock(&dev->frame_lock);
    if (!dev->frame_buf || dev->frame_buf_size < len) {
        if (dev->frame_buf)
            dma_free_coherent(&dev->pdev->dev, dev->frame_buf_size,
                              dev->frame_buf, dev->frame_buf_phys);
        dev->frame_buf = NULL;
        dev->frame_buf_size = 0;

        dev->frame_buf = dma_alloc_coherent(&dev->pdev->dev, len,
                                            &dev->frame_buf_phys,
                                            GFP_KERNEL);
        if (!dev->frame_buf) {
            mutex_unlock(&dev->frame_lock);
            return -ENOMEM;
        }
        dev->frame_buf_size = len;
    }

    ret = c985_dma_read_frame_mode(dev, (u32)addr, dev->frame_buf_phys,
                                   (u32)len, (u32)width, chroma != 0);
	if (!ret) {
		dev->frame_len = len;
		dev->frame_valid = true;
		dev_dbg(&dev->pdev->dev,
			"frame_mb: card=0x%08lx len=%lu w=%lu chroma=%lu ok\n",
			addr, len, width, chroma);
	} else {
        dev_err(&dev->pdev->dev,
                "frame_mb: card=0x%08lx len=%lu failed: %d\n", addr, len, ret);
    }
    mutex_unlock(&dev->frame_lock);

    return ret ? ret : count;
}

static const struct file_operations c985_debugfs_frame_mb_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_frame_mb_write,
    .read = c985_debugfs_frame_read,
    .open = simple_open,
};

/* ---- Unsolicited 0x40 drain: capture + consume + auto-release ---- */
static ssize_t c985_debugfs_mbox_drain(struct file *file,
                                       const char __user *buf,
                                       size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    int ret = c985_mbox_drain(dev);

    return ret < 0 ? ret : count;
}

static const struct file_operations c985_debugfs_mbox_drain_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_mbox_drain,
    .open = simple_open,
};

/* ---- Blocking wait for ARM->host doorbell + mailbox read ---- */
static ssize_t c985_debugfs_mbox_wait(struct file *file,
                                      const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    struct c985_dev *dev = file->private_data;
    char kbuf[32];
    unsigned long timeout_ms = 5000;
    int ret;

    if (count >= sizeof(kbuf))
        return -EINVAL;
    memset(kbuf, 0, sizeof(kbuf));
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;

    if (*kbuf)
        timeout_ms = simple_strtoul(kbuf, NULL, 0);

    ret = c985_mbox_wait_and_read(dev, timeout_ms);
    return ret < 0 ? ret : count;
}

static const struct file_operations c985_debugfs_mbox_wait_fops = {
    .owner = THIS_MODULE,
    .write = c985_debugfs_mbox_wait,
    .open = simple_open,
};

static int c985_debugfs_counters_show(struct seq_file *m, void *v)
{
    struct c985_dev *dev = m->private;
    struct c985_stream_stats vs, as;

    c985_v4l2_get_stats(dev, &vs);
    c985_audio_get_stats(dev, &as);

    seq_printf(m, "=== video (v4l2) ===\n");
    seq_printf(m, "frames_done:    %lld\n", (long long)atomic64_read(&vs.frames_done));
    seq_printf(m, "frames_dropped: %lld\n", (long long)atomic64_read(&vs.frames_dropped));
    seq_printf(m, "desc_non40:     %lld\n", (long long)atomic64_read(&vs.desc_non40));
    seq_printf(m, "desc_bad:       %lld\n", (long long)atomic64_read(&vs.desc_bad));
    seq_printf(m, "no_buf:         %lld\n", (long long)atomic64_read(&vs.no_buf));

    seq_printf(m, "\n=== audio (alsa) ===\n");
    seq_printf(m, "frames_done:    %lld\n", (long long)atomic64_read(&as.frames_done));
    seq_printf(m, "frames_dropped: %lld\n", (long long)atomic64_read(&as.frames_dropped));
    seq_printf(m, "bytes_done:     %lld\n", (long long)atomic64_read(&as.bytes_done));

    seq_printf(m, "\n=== frame fifo ===\n");
    seq_printf(m, "pushed: %d\n", atomic_read(&dev->frame_fifo.frames));
    seq_printf(m, "overflow: %d\n", atomic_read(&dev->frame_fifo.overflow));
    return 0;
}

static int c985_debugfs_counters_open(struct inode *inode, struct file *file)
{
    return single_open(file, c985_debugfs_counters_show, inode->i_private);
}

static const struct file_operations c985_debugfs_counters_fops = {
    .owner = THIS_MODULE,
    .open = c985_debugfs_counters_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

int c985_debugfs_init(struct c985_dev *dev)
{
    dev->debugfs_dir = debugfs_create_dir("c985", NULL);
    if (dev->debugfs_dir) {
        debugfs_create_file("regs", 0444, dev->debugfs_dir, dev, &c985_debugfs_regs_fops);
        debugfs_create_file("counters", 0444, dev->debugfs_dir, dev, &c985_debugfs_counters_fops);
        debugfs_create_file("write_reg", 0222, dev->debugfs_dir, dev, &c985_debugfs_write_fops);
        debugfs_create_file("read_reg", 0644, dev->debugfs_dir, dev, &c985_debugfs_read_reg_fops);
        debugfs_create_file("mbox_send", 0220, dev->debugfs_dir, dev, &c985_debugfs_mbox_fops);
        debugfs_create_file("mbox_drain", 0220, dev->debugfs_dir, dev,
                            &c985_debugfs_mbox_drain_fops);
        debugfs_create_file("mbox_wait", 0220, dev->debugfs_dir, dev,
                            &c985_debugfs_mbox_wait_fops);
        debugfs_create_file("mailbox_status", 0444, dev->debugfs_dir, dev,
                            &c985_debugfs_mbox_status_fops);
        debugfs_create_file("nuc_status", 0444, dev->debugfs_dir, dev,
                            &c985_debugfs_nuc_fops);
        debugfs_create_file("cpr_peek", 0222, dev->debugfs_dir, dev,
                            &c985_debugfs_cpr_peek_fops);
        debugfs_create_file("frame_read", 0644, dev->debugfs_dir, dev,
                            &c985_debugfs_frame_fops);
        debugfs_create_file("frame_mb", 0644, dev->debugfs_dir, dev,
                            &c985_debugfs_frame_mb_fops);
        dev_info(&dev->pdev->dev, "debugfs at /sys/kernel/debug/c985/\n");
    }
    return 0;
}

void c985_debugfs_cleanup(struct c985_dev *dev)
{
    debugfs_remove_recursive(dev->debugfs_dir);
}