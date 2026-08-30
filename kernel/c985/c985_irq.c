// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/* ---- PCIe Config Space / MSI/MSI-X Setup ---- */

int c985_setup_msi(struct c985_dev *dev)
{
    int ret;

    ret = pci_enable_msi(dev->pdev);
    if (ret) {
        dev_warn(&dev->pdev->dev, "MSI enable failed: %d\n", ret);
        return ret;
    }

    dev->msi_enabled = true;
    dev->irq = dev->pdev->irq;
    dev_info(&dev->pdev->dev, "MSI enabled, IRQ %d\n", dev->irq);
    return 0;
}

int c985_setup_msix(struct c985_dev *dev)
{
    int i, ret;
    int num_vectors = 1;  /* Device only supports single MSI (IRQ 143, Count=1/1) */

    for (i = 0; i < num_vectors; i++)
        dev->msix_entries[i].entry = i;

    ret = pci_enable_msix_range(dev->pdev, dev->msix_entries, num_vectors, num_vectors);
    if (ret < 0) {
        dev_dbg(&dev->pdev->dev, "MSI-X enable failed: %d\n", ret);
        return ret;
    }

    dev->msix_enabled = true;
    dev->irq = dev->msix_entries[0].vector;
    dev_info(&dev->pdev->dev, "MSI-X enabled with %d vectors, base IRQ %d\n", ret, dev->irq);
    return 0;
}

void c985_teardown_irq(struct c985_dev *dev)
{
    if (dev->msix_enabled) {
        pci_disable_msix(dev->pdev);
        dev->msix_enabled = false;
    } else if (dev->msi_enabled) {
        pci_disable_msi(dev->pdev);
        dev->msi_enabled = false;
    }
}

void c985_quiesce(struct c985_dev *dev)
{
    u32 val;
    u16 cmd;

    /* Kill every interrupt source AT THE DEVICE before free_irq: a live
     * encoder stream otherwise keeps firing while the handler is being
     * torn down (observed D-state hang in c985_remove) */

    /* Global PCIe/DMA interrupt enables (both BARs) */
    val = c985_read_bar0(dev, C985_DMA_GLOBAL_BASE);
    c985_write_bar0(dev, C985_DMA_GLOBAL_BASE, val & ~1u);
    val = c985_read_bar1(dev, C985_PCIE_IRQ_CTRL);
    c985_write_bar1(dev, C985_PCIE_IRQ_CTRL, val & ~1u);

    /* BAR0+0x8030 bit14 = ARM msg enable */
    val = c985_read_bar0(dev, C985_PCIE_ARM_IRQ);
    c985_write_bar0(dev, C985_PCIE_ARM_IRQ, val & ~0x4000u);

    /* HCI DM interrupts */
    c985_write_bar1(dev, C985_HCI_INT_MASK, 0);

    /* Ack any latched W1C status so nothing re-fires */
    val = c985_read_bar0(dev, C985_PCIE_ARM_IRQ);
    if (val & 0xC0000000u)
        c985_write_bar0(dev, C985_PCIE_ARM_IRQ, val & 0xC0000000u);
    val = c985_read_bar1(dev, C985_DOORBELL);
    if (val & 0x01000000u)
        c985_write_bar1(dev, C985_DOORBELL, val & ~0x01000000u);
    val = c985_read_bar1(dev, C985_HCI_INT_STATUS);
    if (val & 0x00070000u)
        c985_write_bar1(dev, C985_HCI_INT_STATUS, val & 0x00070000u);

    /* Last resort: silence legacy INTx at the PCI level */
    pci_read_config_word(dev->pdev, PCI_COMMAND, &cmd);
    cmd |= PCI_COMMAND_INTX_DISABLE;
    pci_write_config_word(dev->pdev, PCI_COMMAND, cmd);

    synchronize_irq(dev->irq);
}

/* ---- PCIe Setup (reference parity) ----
 * The working driver only ever enables: bus master/mem + global PCIe IRQ
 * (BAR0+0x4000 |= 1, CPCIeCntl_EnableInterrupts). Mailbox operation does
 * NOT depend on interrupt programming - firmware runs in POLLING mode.
 */

int c985_program_pcie_bridge(struct c985_dev *dev)
{
    u16 cmd;
    u32 val;

    /* Enable bus mastering and memory space */
    pci_read_config_word(dev->pdev, PCI_COMMAND, &cmd);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_write_config_word(dev->pdev, PCI_COMMAND, cmd);

    /* Global DMA/PCIe IRQ enable in BAR0+0x4000 bit 0 */
    val = c985_read_bar0(dev, C985_DMA_GLOBAL_BASE);
    c985_write_bar0(dev, C985_DMA_GLOBAL_BASE, val | 0x1);

    dev_dbg(&dev->pdev->dev, "PCIe setup complete (global int enable)\n");
    return 0;
}

/* ---- Interrupt Handler ---- */

irqreturn_t c985_isr(int irq, void *dev_id)
{
    struct c985_dev *dev = dev_id;
    u32 handled = 0;
    u32 val;
    int i;

    atomic_inc(&dev->irq_count);

    /* Check PCIe ARM IRQ (BAR0+0x8030) - bits 30,31 = ARM message status (W1C) */
    val = c985_read_bar0(dev, C985_PCIE_ARM_IRQ);
    if (val & 0xC0000000) {  /* bits 30,31 */
        c985_write_bar0(dev, C985_PCIE_ARM_IRQ, val & 0xC0000000);
        handled = 1;
        atomic_inc(&dev->irq_pcie_count);
        dev_dbg(&dev->pdev->dev, "ISR: PCIe ARM status=0x%08x\n", val & 0xC0000000);
    }

    /* Check BAR1+0x30 bit 30 (ARM message) */
    val = c985_read_bar1(dev, C985_STATUS);
    if (val & 0x40000000) {  /* bit 30 */
        c985_write_bar1(dev, C985_STATUS, val & 0x40000000);
        handled = 1;
        atomic_inc(&dev->irq_pcie_count);
        dev_dbg(&dev->pdev->dev, "ISR: BAR1+0x30 ARM msg bit30 set\n");
    }

    /* Check HCI int status (BAR1+0x804 bits16-18, W1C) */
    val = c985_read_bar1(dev, C985_HCI_INT_STATUS);
    if (val & 0x00070000) {
        c985_write_bar1(dev, C985_HCI_INT_STATUS, val & 0x00070000);
        handled = 1;
        atomic_inc(&dev->irq_hci_count);
        dev_dbg(&dev->pdev->dev, "ISR: HCI status=0x%08x\n", val & 0x00070000);
    }

    /* Check ARM->host doorbell (BAR1+0x24 bit24) - firmware vector 1 */
    val = c985_read_bar1(dev, C985_DOORBELL);
    if (val & 0x01000000) {
        /* Clear bit24 (ARM->host), preserve bit25 (host->ARM) */
        c985_write_bar1(dev, C985_DOORBELL, val & ~0x01000000);
        handled = 1;
        atomic_inc(&dev->irq_doorbell_count);
        dev->doorbell_pending = true;
        wake_up_all(&dev->doorbell_wq);
    }

    /* Check mailbox HIU interrupt (BAR1+0x600 bit16 - HIU mailbox ready)
     * Count/wake only: do NOT write back - firmware owns 0x600 in polling mode */
    val = c985_read_bar1(dev, C985_MAILBOX_CTRL);
    if (val & 0x00010000) {
        handled = 1;
        atomic_inc(&dev->irq_mbox_hiu_count);
        wake_up_all(&dev->mbox_wq);
    }

    /* Check mailbox enable/status (BAR1+0x700 bits 16,17 - firmware vector 6) */
    val = c985_read_bar1(dev, C985_MAILBOX_STATUS);
    if (val & 0x00030000) {  /* bits 16,17 */
        c985_write_bar1(dev, C985_MAILBOX_STATUS, val & 0x00030000);
        handled = 1;
        atomic_inc(&dev->irq_bar1_700_count);
        dev_dbg(&dev->pdev->dev, "ISR: BAR1+0x700 status=0x%08x\n", val & 0x00030000);
    }

    /* Check doorbell E04 (BAR1+0xE04 bits 16,17 - HIU/SW1 / firmware vectors 17/25) */
    val = c985_read_bar1(dev, C985_DOORBELL_E04);
    if (val & 0x00030000) {  /* bits 16,17 */
        c985_write_bar1(dev, C985_DOORBELL_E04, val & 0x00030000);
        handled = 1;
        atomic_inc(&dev->irq_bar1_e04_count);
        dev_dbg(&dev->pdev->dev, "ISR: BAR1+0xE04 status=0x%08x\n", val & 0x00030000);
    }

    /* Check DMA engine completion: legacy semantics require bits 0|1 BOTH
     * set; acknowledge by writing 0x03 back (W1C) */
    for (i = 0; i < 64; i++) {
        if (!dev->dma_chans[i].in_use)
            continue;
        val = c985_read_bar0(dev, C985_DMA_CHAN_BASE + i * C985_DMA_CHAN_STRIDE + C985_DMA_REG_CTRL);
        if ((val & C985_DMA_STATUS_DONE) == C985_DMA_STATUS_DONE) {
            c985_write_bar0(dev, C985_DMA_CHAN_BASE + i * C985_DMA_CHAN_STRIDE + C985_DMA_REG_CTRL,
                            C985_DMA_STATUS_DONE);
            handled = 1;
            atomic_inc(&dev->irq_dma_count);
            complete(&dev->dma_chans[i].done);
            /* Async frame-mode read: advance Y/U/V state machine on the
             * read channel's in-flight frame op (process context). */
            if (i == dev->dma_read_chan && dev->dma_cur_op &&
                dev->dma_frame_wq)
                queue_work(dev->dma_frame_wq, &dev->dma_cur_op->work);
        }
    }

    return handled ? IRQ_HANDLED : IRQ_NONE;
}