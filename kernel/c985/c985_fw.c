// SPDX-License-Identifier: GPL-2.0
#include "c985.h"

/*
 * Firmware boot sequence ported byte-for-byte from the reference driver
 * (qphci.c, cqlcodec.c, firmware.c, qpfwapi.c).
 *
 * KEY DIFFERENCE vs earlier attempts: the firmware is configured for
 * POLLING MODE post-firmware-load (QPSOS config 0x2F1090 = 0). The ARM
 * polls TO_ARM_MSG_STATUS (0x6CC) and clears it per command; host-side
 * interrupts are NOT required for mailbox operation.
 */

/* Register aliases (BAR1) matching reference qphci.h naming */
#define C985_ARM_STATUS         0x800   /* REG_ARM_STATUS: halt handshake polls to 0 */
#define C985_ARM_RESET          0x010   /* REG_ARM_RESET */
#define C985_ARM_TIMER_VAL      0x018   /* REG_ARM_TIMER_VAL */
#define C985_ARM_TIMER_CFG      0x01C   /* REG_ARM_TIMER_CFG */
#define C985_MEM_WIN_BASE       0x81C   /* REG_MEM_WIN_BASE (NOT 0xF14 = DDR ctrl!) */

#define QPHCI_PAGE_SIZE         0x100000

/* QPSOS firmware image layout */
#define QPSOS_SIGNATURE         0x534F5351
#define QPSOS_SIG_OFFSET        0x100
#define QPSOS_VER_OFFSET        0x106
#define QPSOS_CFG_BASE_V2       0x2F2000
#define QPSOS_CFG_BASE_V3       0x0F2000

/* Firmware mode configuration (reference avermedia_c985.c getInitData) */
#define FW_FIXED_MODE           1
#define FW_INT_MODE_POLLING     0

/* PLL configuration (reference avermedia_c985.h; m_Pll4/m_Pll5 default 0) */
#define C985_PLL4_REG           0x0C8
#define C985_PLL5_REG           0x0CC
#define PLL4_VAL_10020          0x00030130
#define PLL4_VAL_DEFAULT        0x00020236
#define PLL5_VAL_DEFAULT        0x00010239

/* Card RAM layout */
#define CARD_RAM_AUDIO_END      0x00170624

/* DDR controller registers (BAR1) */
#define DDR_F14                 0x0F14
#define DDR_F1C                 0x0F1C

/* ---- ARM Control: DM_ResetArm ---- */

static int c985_dm_reset_arm(struct c985_dev *dev, int run)
{
    u32 val;
    unsigned long timeout;

    dev_dbg(&dev->pdev->dev, "DM_ResetArm(run=%d)\n", run);

    if (run == 0) {
        c985_write_bar1(dev, C985_CHIP_CONTROL, 0x00000000);
        c985_write_bar1(dev, C985_ARM_BOOT, 0x00000000);
        c985_write_bar1(dev, C985_ARM_STATUS, 0x00000001);
        c985_write_bar1(dev, C985_ARM_RESET, 0x00000001);

        val = c985_read_bar1(dev, C985_ARM_TIMER_CFG);
        val += 0xFFFF;
        c985_write_bar1(dev, C985_ARM_TIMER_VAL, val);

        c985_write_bar1(dev, C985_ARM_RESET, 0x00000108);

        msleep(15);

        timeout = jiffies + msecs_to_jiffies(3000);
        do {
            val = c985_read_bar1(dev, C985_ARM_STATUS);
            if (val == 0)
                break;
            if (time_after(jiffies, timeout)) {
                dev_err(&dev->pdev->dev, "DM_ResetArm() FAILED! status=0x%x\n", val);
                return -ETIMEDOUT;
            }
            udelay(10);
        } while (1);

        c985_write_bar1(dev, C985_ARM_RESET, 0x00000000);
    }

    c985_write_bar1(dev, C985_TO_ARM_MSG_STATUS, 0x00000000);

    c985_write_bar1(dev, C985_ARM_BOOT, run ? 0x00000001 : 0x00000000);

    dev_dbg(&dev->pdev->dev, "DM_ResetArm(run=%d) done\n", run);
    return 0;
}

/* ---- QPHCI Init ---- */

static void c985_set_mem_windows(struct c985_dev *dev)
{
    int i;

    for (i = 0; i < 3; i++) {
        u32 offset = i * QPHCI_PAGE_SIZE;
        u32 start = offset + 0x4000;
        u32 end = start + QPHCI_PAGE_SIZE - 1;
        u32 base = i * 0x0C;

        c985_write_bar1(dev, C985_MEM_WIN_BASE + base + 0x00, start);
        c985_write_bar1(dev, C985_MEM_WIN_BASE + base + 0x04, end);
        c985_write_bar1(dev, C985_MEM_WIN_BASE + base + 0x08, offset);
    }
}

int c985_qphci_init(struct c985_dev *dev)
{
    u32 val;
    int ret;

    dev_dbg(&dev->pdev->dev, "QPHCI_Init...\n");

    /* Read chip version */
    val = c985_read_bar1(dev, C985_CHIP_VER);
    dev_dbg(&dev->pdev->dev, "Chip version: 0x%08x\n", val);

    /* QPHCI_PowerUp: pad control clear bit8, DDR control, halt ARM */
    val = c985_read_bar1(dev, 0x50);
    val &= 0xFFFFFEFF;
    c985_write_bar1(dev, 0x50, val);

    c985_write_bar1(dev, DDR_F1C, 0x00000F00);

    ret = c985_dm_reset_arm(dev, 0);
    if (ret)
        return ret;

    /* Set up 3 memory mapping windows */
    c985_set_mem_windows(dev);

    /* Magic control writes to enable HCI engine */
    c985_write_bar1(dev, C985_MEM_CTL, 0x70003124);
    c985_write_bar1(dev, C985_MEM_CTL, 0x90003124);

    dev_dbg(&dev->pdev->dev, "QPHCI_Init done\n");
    return 0;
}

/* ---- Memory Controller Init (CQLCodec_InitializeMemory) ---- */

static int c985_cpr_write_chk(struct c985_dev *dev, u32 addr, u32 data)
{
    u32 status;
    int ret = c985_cpr_write(dev, addr, data, &status);
    if (ret)
        dev_err(&dev->pdev->dev, "CPR write 0x%06x failed (status=0x%08x)\n",
                addr, status);
    return ret;
}

static int c985_cpr_read_chk(struct c985_dev *dev, u32 addr, u32 *data)
{
    u32 status;
    int ret = c985_cpr_read(dev, addr, data, &status);
    if (ret)
        dev_err(&dev->pdev->dev, "CPR read 0x%06x failed (status=0x%08x)\n",
                addr, status);
    return ret;
}

int c985_memory_init(struct c985_dev *dev)
{
    u32 rows = 7, cols = 2, f14, rd;
    int ret;

    /* Initial value */
    f14 = 0x20007;
    c985_write_bar1(dev, DDR_F14, f14);
    ret = c985_cpr_write_chk(dev, 0, f14);
    if (ret)
        return ret;

    /* Row address detection */
    for (; rows > 3; rows--) {
        u32 addr = 1 << ((rows + 6) & 0x1f);
        ret = c985_cpr_write_chk(dev, addr, rows - 1);
        if (ret)
            return ret;
    }

    ret = c985_cpr_read_chk(dev, 0, &rd);
    if (ret)
        return ret;
    rows = rd & 0xf;
    f14 = (2 << 16) | rows;
    c985_write_bar1(dev, DDR_F14, f14);

    /* Column address detection */
    ret = c985_cpr_write_chk(dev, 0, cols);
    if (ret)
        return ret;

    for (; cols > 1; cols--) {
        u32 addr = 1 << ((rows + 0x15) & 0x1f);
        ret = c985_cpr_write_chk(dev, addr, cols - 1);
        if (ret)
            return ret;
    }

    ret = c985_cpr_read_chk(dev, 0, &rd);
    if (ret)
        return ret;
    cols = rd & 0xf;
    f14 = (cols << 16) | rows;
    c985_write_bar1(dev, DDR_F14, f14);

    dev_dbg(&dev->pdev->dev, "DDR geometry: rows=%u cols=%u f14=0x%08x\n",
             rows, cols, f14);

    /* Final memory controller configuration */
    {
        u32 f1c = c985_read_bar1(dev, DDR_F1C);
        c985_write_bar1(dev, DDR_F1C, f1c & 0xFFFFFCFF);
    }

    c985_write_bar1(dev, 0xF04, 0x0D03110B);
    c985_write_bar1(dev, 0xF08, 0x00000003);
    c985_write_bar1(dev, 0xF40, 0x00000002);
    c985_write_bar1(dev, 0xF10, 0x05140080);
    c985_write_bar1(dev, 0xF18, 0x00000001);

    msleep(100);

    /* CPR sanity tests */
    ret = c985_cpr_write_chk(dev, 0, 0xAAAAAAAA);
    if (!ret) {
        ret = c985_cpr_read_chk(dev, 0, &rd);
        if (!ret && rd != 0xAAAAAAAA)
            dev_warn(&dev->pdev->dev, "CPR[0x0] = 0x%08x (expect 0xAAAAAAAA) FAIL\n", rd);
    }
    ret = c985_cpr_write_chk(dev, 0x1000, 0x55555555);
    if (!ret) {
        ret = c985_cpr_read_chk(dev, 0x1000, &rd);
        if (!ret && rd != 0x55555555)
            dev_warn(&dev->pdev->dev, "CPR[0x1000] = 0x%08x (expect 0x55555555) FAIL\n", rd);
    }

    dev_dbg(&dev->pdev->dev, "Memory controller initialized\n");
    return 0;
}

/* ---- QPSOS Configuration ---- */

static int c985_parse_qpsos_header(struct c985_dev *dev,
                                   const struct firmware *fw)
{
    u32 sig;
    u16 version;

    if (fw->size <= QPSOS_VER_OFFSET + 2) {
        dev_warn(&dev->pdev->dev, "Firmware too small for QPSOS header (%zu)\n",
                 fw->size);
        return -EINVAL;
    }

    memcpy(&sig, fw->data + QPSOS_SIG_OFFSET, 4);
    sig = le32_to_cpu(sig);
    if (sig != QPSOS_SIGNATURE) {
        dev_warn(&dev->pdev->dev,
                 "Invalid QPSOS signature 0x%08x (expected 0x%08x)\n",
                 sig, QPSOS_SIGNATURE);
        return -EINVAL;
    }

    memcpy(&version, fw->data + QPSOS_VER_OFFSET, 2);
    version = le16_to_cpu(version);

    dev->qpsos_version = version;
    dev->config_base = (version < 3) ? QPSOS_CFG_BASE_V2 : QPSOS_CFG_BASE_V3;

    dev_dbg(&dev->pdev->dev, "QPSOS v%u, config base 0x%06x\n",
             version, dev->config_base);
    return 0;
}

static void c985_write_qpsos_config(struct c985_dev *dev)
{
    u32 config_base, pll4, pll5;

    /* Default when header parse failed: version stays 0 => legacy layout */
    config_base = dev->config_base ? dev->config_base :
                  ((dev->qpsos_version < 3) ? QPSOS_CFG_BASE_V2 : QPSOS_CFG_BASE_V3);
    dev->config_base = config_base;

    c985_cpr_write_chk(dev, 0x2F1094, FW_FIXED_MODE);
    c985_cpr_write_chk(dev, 0x2F1090, FW_INT_MODE_POLLING);
    c985_cpr_write_chk(dev, config_base + 4, 0);

    dev_dbg(&dev->pdev->dev,
             "QPSOS config: 0x2F1094=%u (FixedMode), 0x2F1090=%u (IntMode=POLLING), 0x%06x=0\n",
             FW_FIXED_MODE, FW_INT_MODE_POLLING, config_base + 4);

    /* PLL configuration for PCIe (no overrides; chipver selects) */
    pll4 = (c985_read_bar1(dev, C985_CHIP_VER) == 0x10020) ?
           PLL4_VAL_10020 : PLL4_VAL_DEFAULT;
    pll5 = PLL5_VAL_DEFAULT;
    c985_write_bar1(dev, C985_PLL4_REG, pll4);
    c985_write_bar1(dev, C985_PLL5_REG, pll5);
    dev_dbg(&dev->pdev->dev, "PLL4 (0xC8)=0x%08x PLL5 (0xCC)=0x%08x\n", pll4, pll5);

    /* Clear mailbox */
    c985_write_bar1(dev, C985_TO_ARM_MSG_STATUS, 0);
}

/* ---- DMA helpers ---- */

static int c985_dma_upload(struct c985_dev *dev, dma_addr_t host_dma,
                           u32 card_addr, size_t len, const char *name)
{
    int ret;

    if (c985_dma_alloc_desc(dev, dev->dma_write_chan, 2) != 0) {
        dev_err(&dev->pdev->dev, "%s: no DMA descriptor available\n", name);
        return -EBUSY;
    }

    ret = c985_dma_submit(dev, dev->dma_write_chan, host_dma, card_addr, len, true);
    if (!ret)
        ret = c985_dma_wait(dev, dev->dma_write_chan, 5000);
    c985_dma_free_desc(dev, dev->dma_write_chan);

    if (ret)
        dev_err(&dev->pdev->dev, "%s DMA upload failed: %d\n", name, ret);
    else
        dev_dbg(&dev->pdev->dev, "%s DMA upload complete (%zu bytes to 0x%06x)\n",
                 name, len, card_addr);
    return ret;
}

static int c985_zero_fill_audio_region(struct c985_dev *dev, size_t audio_size)
{
    u32 audio_aligned = ALIGN(audio_size, 4);
    u32 zero_start = FW_AUDIO_OFFSET + audio_aligned;
    u32 zero_bytes = CARD_RAM_AUDIO_END - zero_start;
    u32 chunk = min_t(u32, zero_bytes, 0x8000);
    u8 *zero_buf;
    dma_addr_t zero_dma;
    u32 addr;
    int ret = 0;

    if (audio_aligned >= CARD_RAM_AUDIO_END - FW_AUDIO_OFFSET) {
        dev_warn(&dev->pdev->dev, "Audio firmware too large, skipping zero-fill\n");
        return 0;
    }

    zero_buf = dma_alloc_coherent(&dev->pdev->dev, chunk, &zero_dma, GFP_KERNEL);
    if (!zero_buf) {
        dev_warn(&dev->pdev->dev, "No zero buffer, skipping audio zero-fill\n");
        return 0;
    }

    dev_dbg(&dev->pdev->dev, "Zero-fill %u bytes (0x%06x to 0x%06x)\n",
             zero_bytes, zero_start, CARD_RAM_AUDIO_END);

    for (addr = zero_start; addr < CARD_RAM_AUDIO_END; ) {
        u32 this_chunk = min_t(u32, chunk, CARD_RAM_AUDIO_END - addr);
        ret = c985_dma_upload(dev, zero_dma, addr, this_chunk, "zero-fill");
        if (ret)
            break;
        addr += this_chunk;
    }

    dma_free_coherent(&dev->pdev->dev, chunk, zero_buf, zero_dma);
    return ret;
}

/* ---- Diagnostics ---- */

/* NOTE: 0x4C998 is a write-once init constant, not a counter (TICK-SCHED).
 * Live timekeeping = HW free-runner rollover counters at 0x4C988/0x4C98C. */
void c985_boot_diag(struct c985_dev *dev, const char *when)
{
    u32 intmode = 0, modecfg = 0, roll_a = 0, roll_b = 0;

    c985_cpr_read_chk(dev, 0x2F1090, &intmode);
    c985_cpr_read_chk(dev, 0x55468, &modecfg);
    if (!c985_cpr_read_chk(dev, 0x4C988, &roll_a)) {
        msleep(50);
        c985_cpr_read_chk(dev, 0x4C988, &roll_b);
    }

    dev_dbg(&dev->pdev->dev,
             "bootdiag[%s]: IntMode@0x2F1090=%u modecfg@0x55468=0x%08x rollover@0x4C988 %u->%u (%s)\n",
             when, intmode, modecfg, roll_a, roll_b,
             (roll_a != roll_b) ? "LIVE TIMEKEEPING" : "no rollovers in 50ms");
}

/* Task state table: base 0x6E730, stride 0x5C; +0x2C=active flag,
 * +0x58=gate checked by DTM early-drop */
void c985_task_state_dump(struct c985_dev *dev, u8 task_id)
{
    u32 base = 0x6E730u + (u32)task_id * 0x5Cu;
    u32 w0 = 0, act = 0, gate = 0;

    c985_cpr_read_chk(dev, base, &w0);
    c985_cpr_read_chk(dev, base + 0x2C, &act);
    c985_cpr_read_chk(dev, base + 0x58, &gate);

    dev_dbg(&dev->pdev->dev,
             "task[%u] @0x%06x: w0=0x%08x active(+0x2C)=0x%08x gate(+0x58)=0x%08x\n",
             task_id, base, w0, act, gate);
}

int c985_cpr_verify_spot(struct c985_dev *dev, const u8 *data, size_t size,
                         u32 card_addr, const char *name)
{
    size_t off[3];
    u32 want, got, status;
    int i, bad = 0;

    off[0] = 0;
    off[1] = size >= 8 ? ((size / 2) & ~3u) : 0;
    off[2] = size >= 4 ? size - 4 : 0;

    for (i = 0; i < 3; i++) {
        memcpy(&want, data + off[i], 4);
        want = le32_to_cpu(want);
        if (c985_cpr_read(dev, card_addr + off[i], &got, &status)) {
            dev_warn(&dev->pdev->dev, "%s spot verify: CPR read failed at 0x%06zx\n",
                     name, off[i]);
            bad++;
            continue;
        }
        if (got != want) {
            dev_err(&dev->pdev->dev,
                    "%s MISMATCH at card 0x%08x+0x%06zx: card=0x%08x fw=0x%08x\n",
                    name, card_addr, off[i], got, want);
            bad++;
        }
    }

    if (!bad)
        dev_dbg(&dev->pdev->dev, "%s FW spot verify PASS (3 words @0x%06x)\n",
                 name, card_addr);
    return bad ? -EIO : 0;
}

/* ---- Firmware Upload ---- */

int c985_firmware_load(struct c985_dev *dev)
{
    const struct firmware *fw_video, *fw_audio;
    int ret;
    u32 val;
    u8 *video_buf, *audio_buf;

    dev_dbg(&dev->pdev->dev, "Loading firmware...\n");

    ret = request_firmware(&fw_video, "avermedia/qpvidfwpcie.bin", &dev->pdev->dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "Failed to load video firmware: %d\n", ret);
        return ret;
    }

    ret = request_firmware(&fw_audio, "avermedia/qpaudfw.bin", &dev->pdev->dev);
    if (ret) {
        dev_warn(&dev->pdev->dev, "Failed to load audio firmware: %d (continuing)\n", ret);
        fw_audio = NULL;
    }

    dev->fw_video_size = fw_video->size;
    dev->fw_audio_size = fw_audio ? fw_audio->size : 0;

    /* Parse QPSOS version from VIDEO firmware (non-fatal) */
    c985_parse_qpsos_header(dev, fw_video);

    /* Halt ARM */
    ret = c985_dm_reset_arm(dev, 0);
    if (ret)
        goto err_release_fw;

    /* QPHCI Init (memory windows at 0x81C, MEM_CTL magic) */
    ret = c985_qphci_init(dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "QPHCI_Init failed: %d\n", ret);
        goto err_release_fw;
    }

    /* Memory init (DDR row/col detect via CPR) */
    ret = c985_memory_init(dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "Memory init failed: %d\n", ret);
        goto err_release_fw;
    }

    /* CQLCodec_SetGPIODefaults (ao_vo_gpio) - enables mailbox interrupt path
     * Windows sequence: DDR init -> SetGPIODefaults -> ARM release
     * Sets: 0x50 clear bit1 (AOEnable), set bit2 (VOEnable)
     *       0x610 = 0 (GPIODirections), 0x614 = 0 (GPIOValues) */
    c985_write_bar1(dev, 0x50, (c985_read_bar1(dev, 0x50) & ~0x2) | 0x4);
    c985_write_bar1(dev, 0x610, 0);
    c985_write_bar1(dev, 0x614, 0);
    dev_dbg(&dev->pdev->dev, "GPIO defaults applied (0x50, 0x610, 0x614)\n");

    /* Copy firmware to DMA-coherent buffers (request_firmware returns vmalloc memory) */
    video_buf = dma_alloc_coherent(&dev->pdev->dev, fw_video->size,
                                    &dev->fw_video_dma, GFP_KERNEL);
    if (!video_buf) {
        dev_err(&dev->pdev->dev, "Failed to allocate DMA buffer for video firmware\n");
        ret = -ENOMEM;
        goto err_release_fw;
    }
    memcpy(video_buf, fw_video->data, fw_video->size);

    audio_buf = NULL;
    if (fw_audio) {
        audio_buf = dma_alloc_coherent(&dev->pdev->dev, fw_audio->size,
                                        &dev->fw_audio_dma, GFP_KERNEL);
        if (!audio_buf) {
            dev_err(&dev->pdev->dev, "Failed to allocate DMA buffer for audio firmware\n");
            ret = -ENOMEM;
            goto err_free_video;
        }
        memcpy(audio_buf, fw_audio->data, fw_audio->size);
    }

    /* STEP: Upload AUDIO firmware first (0x100000), then zero-fill remainder */
    if (fw_audio) {
        ret = c985_dma_upload(dev, dev->fw_audio_dma, FW_AUDIO_OFFSET,
                              fw_audio->size, "audio FW");
        if (ret)
            goto err_free_all;
        c985_cpr_verify_spot(dev, fw_audio->data, fw_audio->size,
                             FW_AUDIO_OFFSET, "audio");
        msleep(1);

        ret = c985_zero_fill_audio_region(dev, fw_audio->size);
        if (ret)
            goto err_free_all;
    } else {
        dev_warn(&dev->pdev->dev, "No audio firmware, skipping audio upload\n");
    }

    /* STEP: Upload video firmware (0x0) */
    ret = c985_dma_upload(dev, dev->fw_video_dma, FW_VIDEO_OFFSET,
                          fw_video->size, "video FW");
    if (ret)
        goto err_free_all;
    c985_cpr_verify_spot(dev, fw_video->data, fw_video->size,
                         FW_VIDEO_OFFSET, "video");

    /* STEP: QPSOS configuration - POLLING MODE post-firmware-load */
    c985_write_qpsos_config(dev);

    /* STEP: Pre-boot delay */
    msleep(250);

    /* STEP: Start ARM (release = clear mailbox + boot=1) */
    ret = c985_dm_reset_arm(dev, 1);
    if (ret)
        goto err_free_all;

    wmb();
    udelay(10);

    /* STEP: Interrupt enable (post-release, reference firmware.c:447) */
    c985_write_bar1(dev, C985_IRQ_MASK, 0x01000000);

    msleep(500);

    /* STEP: Ring ARM boot doorbell ONCE - wakes ARM from WFI
     * (reference qpfwapi.c arm_ring_doorbell: BAR0+0x04, 0 -> 1) */
    c985_write_bar0(dev, 0x04, 0x00000000);
    wmb();
    udelay(100);
    c985_write_bar0(dev, 0x04, 0x00000001);
    wmb();

    msleep(500);

    /* STEP: Firmware communication init sanity (reference QPFWAPI_Init) */
    c985_write_bar1(dev, C985_TO_ARM_MSG_STATUS, 0);
    msleep(100);
    val = c985_read_bar1(dev, C985_TO_ARM_MSG_STATUS);
    if (val & 1)
        dev_warn(&dev->pdev->dev,
                 "Mailbox sanity: 0x6CC=0x%08x bit0 STUCK - ARM may not be alive\n", val);
    else
        dev_dbg(&dev->pdev->dev,
                 "Mailbox sanity: 0x6CC bit0 clear - ARM polling mailbox OK\n");

    c985_boot_diag(dev, val & 1 ? "post-release-STUCK" : "post-release");

    /* NOTE: dispatch tables (0x6B454/0x6B654) are NOT host-populated -
     * Windows ground truth never writes them; they are filled by firmware
     * DTM registration during SystemOpen/SystemLink processing.
     * Runtime command order per AVerPL33_x64.sys: F1 -> F2 -> 0x11
     * SetEncMode -> 0x06 UpdateConfig -> 0x01 StartEncoder. */

    /* Global PCIe interrupt enable (reference CPCIeCntl_EnableInterrupts) */
    val = c985_read_bar0(dev, C985_DMA_GLOBAL_BASE);
    c985_write_bar0(dev, C985_DMA_GLOBAL_BASE, val | 0x1);

    dev->fw_loaded = true;
    dev->arm_running = true;

    dma_free_coherent(&dev->pdev->dev, fw_video->size, video_buf, dev->fw_video_dma);
    release_firmware(fw_video);
    if (fw_audio) {
        dma_free_coherent(&dev->pdev->dev, fw_audio->size, audio_buf, dev->fw_audio_dma);
        release_firmware(fw_audio);
    }

    dev_info(&dev->pdev->dev, "Firmware loaded, ARM running in POLLING mode\n");
    return 0;

err_free_all:
    dma_free_coherent(&dev->pdev->dev, fw_video->size, video_buf, dev->fw_video_dma);
    if (fw_audio)
        dma_free_coherent(&dev->pdev->dev, fw_audio->size, audio_buf, dev->fw_audio_dma);
err_free_video:
    release_firmware(fw_video);
    if (fw_audio)
        release_firmware(fw_audio);
    return ret;

err_release_fw:
    release_firmware(fw_video);
    if (fw_audio)
        release_firmware(fw_audio);
    return ret;
}
