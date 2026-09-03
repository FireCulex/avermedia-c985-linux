// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

static bool auto_release;
module_param(auto_release, bool, 0444);
MODULE_PARM_DESC(auto_release, "send opcode 0x30 release after each 0x40 frame-done (default: disabled)");

static const struct pci_device_id c985_ids[] = {
    { PCI_DEVICE(C985_VENDOR, C985_DEVICE) },
    { }
};
MODULE_DEVICE_TABLE(pci, c985_ids);

static struct pci_driver c985_driver = {
    .name = "c985",
    .id_table = c985_ids,
    .probe = c985_probe,
    .remove = c985_remove,
};

int c985_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct c985_dev *dev;
    int err;
    u32 val;

    dev_dbg(&pdev->dev, "C985 driver probing...\n");

    dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    dev->pdev = pdev;
    dev->auto_release = auto_release;
    mutex_init(&dev->lock);
    mutex_init(&dev->dma_read_lock);
    mutex_init(&dev->frame_lock);
    spin_lock_init(&dev->irq_lock);
    init_waitqueue_head(&dev->doorbell_wq);
    init_waitqueue_head(&dev->mbox_wq);
    init_waitqueue_head(&dev->dma_wq);
    dev->doorbell_pending = false;

    dev->mbox_drain_wq = alloc_ordered_workqueue("c985-mbox", 0);
    if (!dev->mbox_drain_wq) {
        err = -ENOMEM;
        goto err_regions;
    }

    spin_lock_init(&dev->frame_fifo.lock);
    INIT_WORK(&dev->mbox_drain_work, c985_mbox_drain_work_fn);
    atomic_set(&dev->frame_fifo.overflow, 0);
    atomic_set(&dev->frame_fifo.frames, 0);
    dev->streaming = false;
    dev->frame_consumer = NULL;
    atomic_set(&dev->irq_count, 0);
    atomic_set(&dev->irq_pcie_count, 0);
    atomic_set(&dev->irq_hci_count, 0);
    atomic_set(&dev->irq_doorbell_count, 0);
    atomic_set(&dev->irq_mbox_hiu_count, 0);
    atomic_set(&dev->irq_mbox_sw1_count, 0);
    atomic_set(&dev->irq_bar1_700_count, 0);
    atomic_set(&dev->irq_bar1_e04_count, 0);
    atomic_set(&dev->irq_dma_count, 0);

    err = pcim_enable_device(pdev);
    if (err) {
        dev_err(&pdev->dev, "pcim_enable_device failed: %d\n", err);
        return err;
    }

    err = pci_request_regions(pdev, "c985");
    if (err) {
        dev_err(&pdev->dev, "pci_request_regions failed: %d\n", err);
        return err;
    }

    /* Map BARs early for IRQ setup */
    dev->bar0_phys = pci_resource_start(pdev, 0);
    dev->bar1_phys = pci_resource_start(pdev, 1);

    dev_info(&pdev->dev, "BAR0/BAR1 mapped: BAR0=0x%llx len=%llu BAR1=0x%llx len=%llu\n", 
             (unsigned long long)dev->bar0_phys, (unsigned long long)pci_resource_len(pdev, 0),
             (unsigned long long)dev->bar1_phys, (unsigned long long)pci_resource_len(pdev, 1));

    dev->bar0 = devm_ioremap(&pdev->dev, dev->bar0_phys, pci_resource_len(pdev, 0));
    if (!dev->bar0) {
        err = -ENOMEM;
        goto err_regions;
    }

    dev->bar1 = devm_ioremap(&pdev->dev, dev->bar1_phys, pci_resource_len(pdev, 1));
    if (!dev->bar1) {
        err = -ENOMEM;
        goto err_regions;
    }

    /* Try MSI-X first, then MSI, then INTx */
    err = c985_setup_msix(dev);
    if (err < 0) {
        err = c985_setup_msi(dev);
        if (err < 0) {
            pci_intx(pdev, 1);
            dev_info(&pdev->dev, "Using INTx, IRQ %d\n", pdev->irq);
            dev->irq = pdev->irq;
        }
    }

    /* Verify chip version */
    val = c985_read_bar1(dev, C985_CHIP_VER);
    if (val != C985_CHIP_VER_EXPECT) {
        dev_err(&pdev->dev, "Chip version mismatch: 0x%08x (expect 0x%08x)\n",
                val, C985_CHIP_VER_EXPECT);
        err = -ENODEV;
        goto err_regions;
    }

    val = c985_read_bar1(dev, 0x2C);  /* Magic */
    if (val != C985_MAGIC_EXPECT) {
        dev_warn(&pdev->dev, "Magic mismatch: 0x%08x (expect 0x%08x)\n",
                 val, C985_MAGIC_EXPECT);
    }

    /* Program PCIe bridge for mailbox interrupt path */
    c985_program_pcie_bridge(dev);

    /* Register interrupt handler */
    err = devm_request_irq(&pdev->dev, dev->irq, c985_isr,
                           dev->msi_enabled || dev->msix_enabled ? 0 : IRQF_SHARED,
                           "c985", dev);
    if (err) {
        dev_err(&pdev->dev, "request_irq failed: %d\n", err);
        goto err_regions;
    }

    pci_set_master(pdev);

    /* Initialize DMA */
    c985_dma_init(dev);

    /* Load firmware */
    err = c985_firmware_load(dev);
    if (err) {
        dev_err(&dev->pdev->dev, "Firmware load failed: %d\n", err);
        goto err_regions;
    }

    /* Debugfs */
    c985_debugfs_init(dev);

    /* v4l2/vb2 capture device */
    c985_v4l2_init(dev);

    /* ALSA audio capture device */
    c985_audio_init(dev);

    pci_set_drvdata(pdev, dev);

    dev_info(&pdev->dev, "Probe successful, IRQ=%d, MSI=%d, MSI-X=%d\n",
             dev->irq, dev->msi_enabled, dev->msix_enabled);
    return 0;

err_regions:
    devm_free_irq(&pdev->dev, dev->irq, dev);
    pci_release_regions(pdev);
    c985_teardown_irq(dev);
    return err;
}

void c985_remove(struct pci_dev *pdev)
{
    struct c985_dev *dev = pci_get_drvdata(pdev);

    if (!dev)
        return;

    c985_quiesce(dev);
    c985_audio_cleanup(dev);
    c985_v4l2_cleanup(dev);
    c985_debugfs_cleanup(dev);

    if (dev->mbox_drain_wq) {
        cancel_work_sync(&dev->mbox_drain_work);
        destroy_workqueue(dev->mbox_drain_wq);
        dev->mbox_drain_wq = NULL;
    }

    if (dev->frame_buf) {
        dma_free_coherent(&pdev->dev, dev->frame_buf_size, dev->frame_buf,
                          dev->frame_buf_phys);
        dev->frame_buf = NULL;
        dev->frame_buf_size = 0;
    }

    devm_free_irq(&pdev->dev, dev->irq, dev);
    pci_release_regions(pdev);
    c985_teardown_irq(dev);

    dev_dbg(&pdev->dev, "Removed\n");
}

module_pci_driver(c985_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("C985 Project");
MODULE_DESCRIPTION("AverMedia C985 PCIe Capture Card Driver - Minimal");
MODULE_FIRMWARE("avermedia/qpvidfwpcie.bin");
MODULE_FIRMWARE("avermedia/qpaudfw.bin");