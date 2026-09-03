// SPDX-License-Identifier: GPL-2.0
#ifndef _C985_H_
#define _C985_H_

#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/scatterlist.h>

#define C985_VENDOR 0x1AF2
#define C985_DEVICE 0xA001

/* BAR0 (DMA/PCIe controller) - Linux BAR0 at 0xF7D10000 */
#define C985_PCIE_ARM_IRQ       0x8030  /* bit30=ARM msg, bit14=ARM msg enable */
#define C985_DMA_GLOBAL_BASE    0x4000  /* Global DMA control */
#define C985_DMA_GLOBAL_CTRL    0x00    /* bit0=global enable */
#define C985_DMA_CHAN_BASE      0x0000 /* 64 channels at 0x100 stride (BAR0+0x10000) */
#define C985_DMA_CHAN_STRIDE    0x100
#define C985_DMA_REG_CAPS       0x00    /* Capabilities */
#define C985_DMA_REG_CTRL       0x04    /* ControlStatus */
#define C985_DMA_REG_DESC_LO    0x08    /* Descriptor low */
#define C985_DMA_REG_DESC_HI    0x0C    /* Descriptor high */
#define C985_DMA_STATUS_BUSY    0x400   /* Busy bit in ControlStatus */
#define C985_DMA_STATUS_DONE    0x03    /* Bits 0|1 = transfer complete (W1C ack) */
#define C985_DMA_CTRL_START     0x101   /* Start DMA */
#define C985_DMA_DESC_ALIGN     32

/* Per-engine descriptor pool size. Matches Windows PedDmaInit@0xB8460, which
 * does NumDescriptors = 0x1002 (4098) and AllocateCommonBuffer(4098*0x20+0x20).
 * Covers a whole-frame 4KB-granular SG chain (~800 elements) with ample headroom. */
#define C985_DMA_NUM_DESCS      4098

/* Descriptor control words (AVerPL33_x64.sys PedDmaQueueBuffers@0xB8C60):
 * mode 3 linear read vs default linear write */
#define C985_DMA_CTRL_LIN_WRITE 0x0C02100F
#define C985_DMA_CTRL_LIN_READ  0x0C02100F
/* Card-address flags: base-select always set; bit59 = end marker on the
 * LAST descriptor's card address for reads (no dummy descriptor on reads) */
#define C985_DMA_CARD_FLAG      0x400000000ULL
#define C985_DMA_CARD_RD_END    0x0800000000000000ULL

/* BAR1 (Main/Mailbox/ARM) */
#define C985_CHIP_CONTROL       0x000
#define C985_IRQ_STATUS         0x004   /* bit24=ARM->host interrupt enable */
#define C985_IRQ_MASK           0x00C
#define C985_DOORBELL           0x024   /* bit24=ARM->host, bit25=host->ARM */
#define C985_STATUS             0x030
#define C985_CHIP_VER           0x038
#define C985_MAILBOX_CTRL       0x600   /* bit16=mailbox ready (HIU) */
#define C985_MAILBOX_STATUS     0x700   /* bits16,17=mailbox enable / firmware vector 6 */
#define C985_FROM_ARM_MSG_STATUS 0x6C8  /* bit0=valid response */
#define C985_TO_ARM_MSG_STATUS  0x6CC   /* bit0=valid command */
#define C985_TO_ARM_PARAM0      0x6F8   /* payload word 0 (function/link params) */
#define C985_TO_ARM_MESSAGE     0x6FC   /* bits15:0=opcode, bits31:16=taskId */
#define C985_FROM_ARM_MESSAGE   0x6B0
#define C985_HCI_INT_MASK       0x800   /* bits16-18=DM int enable */
#define C985_HCI_INT_STATUS     0x804   /* bits16-18=DM int status (W1C) */
#define C985_ARM_BOOT           0x80C
#define C985_MEM_CTL            0x840
#define C985_PCIE_IRQ_CTRL      0x4000  /* bit0=PCIe ARM IRQ enable */
#define C985_DOORBELL_E04       0xE04   /* bits16,17=Doorbell HIU/SW1 / firmware vectors 17/25 */

/* CPR Registers (BAR1) */
#define C985_CPR_WR_ADDR        0x78C
#define C985_CPR_WR_CTL         0x790
#define C985_CPR_WR_DATA        0x794
#define C985_CPR_RD_ADDR        0x780
#define C985_CPR_RD_CTL         0x784
#define C985_CPR_RD_DATA        0x788

#define C985_CHIP_VER_EXPECT    0x00010020
#define C985_MAGIC_EXPECT       0x45433210

/* Firmware */
#define FW_VIDEO_OFFSET         0x00000000
#define FW_AUDIO_OFFSET         0x00100000
#define FW_VIDEO_SIZE           353236
#define FW_AUDIO_SIZE           362776

/* Encoder task IDs (upper 16 bits of 0x6FC/0x6CC/0x6B0). task0 = video,
 * task1 = audio. Verified against live dbgview.log + AVerPL33_x64.sys. */
#define C985_ENC_TASK           0
#define C985_AUD_TASK           1

/* Encoder function IDs (0x6F8 param for SystemOpen F1). */
#define C985_FUNC_VIDEO         0x80000004
#define C985_FUNC_AUDIO         0x80000040

/* Frame descriptor dataType tags (p1 & 0xffff). Raw outputs only. */
#define C985_DT_RAW_VIDEO       0x81
#define C985_DT_RAW_AUDIO       0x82

/* Encoder config register burst width: 0x6F8 .. 0x6D0 inclusive. */
#define C985_ENC_CFG_REGS       11

/*
 * Audio capture via the compressed path (live OBS ground truth): the card
 * records AAC-LC @48kHz/128kbps/stereo natively (product spec MPEG4
 * H.264+AAC). Audio frames arrive on the SAME 0x40/0x41 EncDataOutReq
 * descriptor as video, tagged taskId=1 in the upper 16 bits of 0x6B0, and
 * are DMA-read LINEARLY (ctrl 0x0804200F, w=0 h=0) in 1536/3072/4608-byte
 * chunks. Raw PCM (RawAudOutput 0x82, ARM_BUF_OTHERS) uses the separate
 * 0xC0 descriptor with 0xA0/0xA1 reply instead.
 */
#define C985_AUDIO_FRAME_MAX    8192    /* generous: AAC frames are <=4608 */

/* Frame readback cap (debugfs frame_read) */
#define C985_FRAME_MAX          (8u * 1024u * 1024u)

/* PCIe Config Space for MSI/MSI-X */
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11

struct c985_dma_desc {
    __le32 ctrl;
    __le32 len;
    __le64 host_addr;
    __le64 card_addr;
    __le64 next_desc;
} __packed;

struct c985_dma_chan {
    void __iomem *regs;
    struct c985_dma_desc *desc_ring;
    dma_addr_t desc_ring_phys;
    int chan_id;
    int desc_count;
    struct completion done;
    bool in_use;
};

struct c985_mbox_result {
    u32 status_sent;        /* word written to 0x6CC */
    u32 msg_written;        /* word written to 0x6FC: (taskId<<16)|opcode */
    u32 param0_written;     /* word written to 0x6F8 */
    u8 task_id;             /* task of the command that produced this */
    s64 clear_us;           /* time for ARM to clear bit0 (-1 = timeout) */
    s64 resp_us;            /* time for 0x6C8 bit0 (-1 = none/timeout) */
    bool resp_ok;
    u32 from_arm_msg;       /* 0x6B0 */
    u32 from_arm_status;    /* 0x6C8 (bit0=valid, bit8=ack-required) */
    u32 resp_params[5];     /* 0x6B4..0x6C4 */
    u32 ack_word;           /* ack message sent if bit8 was set (0 = none) */
};

/*
 * Frame descriptor extracted from the 0x40 mailbox notification.
 * Mirrors the firmware EncDataOutReq layout (Stage 3 of the Windows
 * driver capture flow), host-chewed for DMA dispatch:
 *   p0 = (ring_idx<<24)|tag; p1 = Y_dword_addr; p2 = UV_dword_addr;
 *   p3 = chroma_size_bytes; p4 = PTS (bit31 = pts-valid)
 */
struct c985_frame_desc {
    u32 tag;        /* p0 low bits (0x81 = raw YUV, 0x82 = raw audio) */
    u32 ring_idx;   /* p0 >> 24 */
    u32 y_dw;       /* p1: Y plane / audio buffer card address in DWORDs */
    u32 u_dw;       /* p2: UV plane card address in DWORDs */
    u32 chroma;     /* p3: chroma plane size / audio buffer size in bytes */
    u32 pts_raw;    /* p4: PTS, bit31 = pts-valid */
    u8  task;       /* taskId from 0x6B0 upper 16 bits (0 video / 1 audio) */
    bool valid;
};

/*
 * One pending full-frame DMA op: three serial plane reads (Y, U, V) sequenced
 * by DMA completion, mirroring Windows CTask_BuildIoBlockYUVMB2RAS + the DPC
 * completion model. Planes are read from a vb2_dma_sg scatterlist (one HW
 * descriptor per SG element, PedDmaQueueBuffers style). done_cb fires once
 * after all three planes complete (or a plane fails); the callback owns the
 * vb2 buffer handoff + 0x30 ring-slot release.
 */
struct c985_frame_op;
typedef void (*c985_frame_done_t)(struct c985_frame_op *op);

struct c985_frame_op {
    struct list_head list;
    struct c985_dev *dev;
    struct c985_frame_desc desc;

    /* Scatter-gather backing (vb2_dma_sg plane 0). Plane byte offsets into
     * the sg_table: Y@0, U@C985_Y_LEN, V@C985_Y_LEN+C985_C_LEN. */
    struct sg_table *sgt;

    /* completion state machine: 0=Y in flight, 1=U, 2=V, 3=done */
    u8 phase;
    struct work_struct work;       /* async completion work (scheduled by ISR) */
    c985_frame_done_t done_cb;
    bool failed;
};

/* Frame-mode DMA control words (asm-verified AVerPL33_x64.sys):
 * Y=0x08BE100F (mode1 fm1), U/V=0x4861000F (mode2 fm1) */
#define C985_DMA_CTRL_FRAME_Y    0x08BE100F
#define C985_DMA_CTRL_FRAME_UV   0x4861000F
/* V plane = U plane + 0x10 DWORDs = +64 bytes (card DDR overlap, HW
 * differentiates via OffsetEx mode-2 field). */
#define C985_V_OFFSET_BYTES      0x40

/* Frame geometry: 1920x1080 YUV420. Y=2073600, U=V=518400. */
#define C985_WIDTH               1920
#define C985_HEIGHT              1080
#define C985_Y_LEN               (C985_WIDTH * C985_HEIGHT)
#define C985_C_LEN               (C985_Y_LEN / 4)
#define C985_FRAME_BYTES         (C985_Y_LEN + 2 * C985_C_LEN)

/* Mailbox FIFO for 0x40 frame descriptors (ISR push -> workqueue pop). */
#define C985_FRAME_FIFO_DEPTH    16

struct c985_frame_fifo {
    struct c985_frame_desc ring[C985_FRAME_FIFO_DEPTH];
    spinlock_t lock;
    u32 head;   /* next write slot */
    u32 tail;   /* next read slot */
    u32 count;
    atomic_t overflow;  /* push-dropped count (debugfs/telemetry) */
    atomic_t frames;    /* total pushed (debugfs/telemetry) */
};

struct c985_dev {
    struct pci_dev *pdev;
    struct mutex lock;
    struct mutex dma_read_lock;   /* serializes the single C2S (read) channel */
    spinlock_t irq_lock;

    void __iomem *bar0;  /* DMA/PCIe */
    void __iomem *bar1;  /* Main/Mailbox/ARM */
    u32 bar0_phys;
    u32 bar1_phys;

    struct c985_dma_chan dma_chans[64];
    int dma_write_chan;
    int dma_read_chan;
    spinlock_t dma_cur_op_lock;   /* protects dma_cur_op (ISR/work/submit) */
    struct c985_frame_op *dma_cur_op; /* frame op whose plane is in flight */

    /* Interrupt handling */
    int irq;
    bool msi_enabled;
    bool msix_enabled;
    struct msix_entry msix_entries[8];
    atomic_t irq_count;
    atomic_t irq_pcie_count;
    atomic_t irq_hci_count;
    atomic_t irq_doorbell_count;
    atomic_t irq_mbox_hiu_count;
    atomic_t irq_mbox_sw1_count;
    atomic_t irq_bar1_700_count;
    atomic_t irq_bar1_e04_count;
    atomic_t irq_dma_count;
    struct workqueue_struct *dma_frame_wq; /* DMA frame-op continuation */
    struct workqueue_struct *mbox_drain_wq; /* mailbox FIFO drain */
    wait_queue_head_t doorbell_wq;
    wait_queue_head_t mbox_wq;
    wait_queue_head_t dma_wq;
    bool doorbell_pending;  /* set by ISR when ARM->host doorbell fires */

    /* Firmware state */
    bool fw_loaded;
    bool arm_running;
    u8 *fw_video;
    u8 *fw_audio;
    size_t fw_video_size;
    size_t fw_audio_size;
    dma_addr_t fw_video_dma;
    dma_addr_t fw_audio_dma;

    /* QPSOS firmware configuration */
    u32 qpsos_version;      /* parsed from video FW @0x106 (LE16) */
    u32 config_base;        /* QPSOS config area (v<3: 0x2F2000, v>=3: 0x0F2000) */

    /* Debugfs */
    struct dentry *debugfs_dir;

    /* Mailbox command/response (polling mode) */
    struct c985_mbox_result last_mbox;
    bool have_mbox_result;
    bool auto_release;      /* send opcode 0x30 after each 0x40 frame-done */
    bool releasing;         /* recursion guard for auto-release */
    /* Payload slot window 0x6F4..0x6D0 (index 0 = 0x6F4); 0x6F8 is param0 */
    u32 mbox_slots[10];

    /* Interrupt-driven frame path (v4l2 streaming) */
    struct c985_frame_fifo frame_fifo;
    struct delayed_work mbox_drain_work;
    bool streaming;
    bool streamed_once;     /* a capture session has run since module load */
    /* Frame consumer hook: called from mbox drain work with each popped
     * 0x40 descriptor. Set by the v4l2 layer (Phase 3/4). */
    int (*frame_consumer)(struct c985_dev *dev, struct c985_frame_desc *d);
    void *v4l2_priv;    /* opaque c985_v4l2 state (allocated on demand) */

    /* Audio consumer hook: called from mbox drain work with each popped
     * 0x40/0x41 descriptor whose taskId==C985_AUD_TASK. The audio layer
     * DMA-reads the (compressed AAC) buffer linearly and feeds ALSA.
     * Descriptor fields reused: y_dw = buffer addr (dwords), chroma = size
     * (bytes). Set by the audio (c985_audio) layer. */
    void (*audio_consumer)(struct c985_dev *dev, struct c985_frame_desc *d);
    void *audio_priv;   /* opaque c985_audio state (allocated on demand) */

    /* CPR peek (debugfs) */
    u32 cpr_peek_addr;
    u32 cpr_peek_val;
    bool have_cpr_peek;

    /* Frame readback (debugfs frame_read) */
    struct mutex frame_lock;
    u8 *frame_buf;
    dma_addr_t frame_buf_phys;
    size_t frame_buf_size;
    size_t frame_len;
    bool frame_valid;
};

/* Core functions */
int c985_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void c985_remove(struct pci_dev *pdev);

/* IRQ functions */
int c985_setup_msi(struct c985_dev *dev);
int c985_setup_msix(struct c985_dev *dev);
void c985_teardown_irq(struct c985_dev *dev);
void c985_quiesce(struct c985_dev *dev);
int c985_program_pcie_bridge(struct c985_dev *dev);
irqreturn_t c985_isr(int irq, void *dev_id);

/* DMA functions */
int c985_dma_init(struct c985_dev *dev);
int c985_dma_alloc_desc(struct c985_dev *dev, int chan_id, int num_descs);
void c985_dma_free_desc(struct c985_dev *dev, int chan_id);
int c985_dma_submit(struct c985_dev *dev, int chan_id,
                    dma_addr_t host_addr, u32 card_addr, u32 len, bool write);
int c985_dma_wait(struct c985_dev *dev, int chan_id, unsigned long timeout_ms);
int c985_dma_read_linear(struct c985_dev *dev, u32 card_addr,
                         dma_addr_t host_phys, u32 len);
int c985_dma_read_frame_mode(struct c985_dev *dev, u32 card_addr,
                             dma_addr_t host_phys, u32 len,
                             u32 width, bool chroma);
/* Scatter-gather frame-mode read: walks a vb2_dma_sg sg_table starting at
 * `offset` bytes, emitting one chained descriptor per SG element. Replaces the
 * contiguous-buffer path to remove the order-10 DMA32 fragmentation failure. */
int c985_dma_read_frame_mode_sg(struct c985_dev *dev, u32 card_addr,
                                struct sg_table *sgt, u32 offset, u32 len,
                                u32 width, bool chroma);

/* Async SG frame-op (Y->U->V serial, ISR-driven continuation). */
int c985_dma_read_frame_mode_sg_submit(struct c985_dev *dev, u32 card_addr,
                                       struct sg_table *sgt, u32 offset,
                                       u32 len, u32 width, bool chroma);
int c985_dma_submit_frame(struct c985_frame_op *op);
void c985_dma_frame_next(struct c985_frame_op *op);

/* ARM control */
int c985_qphci_init(struct c985_dev *dev);
int c985_memory_init(struct c985_dev *dev);

/* Firmware */
int c985_firmware_load(struct c985_dev *dev);

/* Mailbox */
int c985_mbox_send_polling(struct c985_dev *dev, u16 opcode, u32 param,
                           u8 task_id, bool has_resp, bool bare,
                           unsigned long timeout_ms);
int c985_mbox_drain(struct c985_dev *dev);
int c985_mbox_wait_and_read(struct c985_dev *dev, unsigned long timeout_ms);
void c985_mbox_release_last(struct c985_dev *dev);
void c985_mbox_release_desc(struct c985_dev *dev, const struct c985_frame_desc *d);

/* Mailbox frame FIFO (interrupt-driven streaming path) */
bool c985_mbox_fifo_push(struct c985_dev *dev, struct c985_frame_desc *d);
bool c985_mbox_fifo_pop(struct c985_dev *dev, struct c985_frame_desc *d);
bool c985_mbox_fifo_push_front(struct c985_dev *dev, struct c985_frame_desc *d);
void c985_mbox_fifo_reset(struct c985_dev *dev);
int c985_mbox_flush_pending(struct c985_dev *dev);

void c985_mbox_isr_service(struct c985_dev *dev);
void c985_mbox_drain_work_fn(struct work_struct *w);

/* Diagnostics */
void c985_boot_diag(struct c985_dev *dev, const char *when);
void c985_task_state_dump(struct c985_dev *dev, u8 task_id);
int c985_cpr_verify_spot(struct c985_dev *dev, const u8 *data, size_t size,
                         u32 card_addr, const char *name);

/* CPR (Card Program Register) */

int c985_cpr_write(struct c985_dev *dev, u32 addr, u32 data, u32 *status);
int c985_cpr_read(struct c985_dev *dev, u32 addr, u32 *data, u32 *status);
int c985_nuc100_status(struct c985_dev *dev, u8 id3[3], u8 *status,
		       u16 *hact, u16 *vact, u16 *htot, u16 *vtot,
		       int *pclk_khz, u8 raw7[7]);


/* Debugfs */
int c985_debugfs_init(struct c985_dev *dev);
void c985_debugfs_cleanup(struct c985_dev *dev);

/* v4l2/vb2 device (Phase 3/4) */
struct video_device;
int c985_v4l2_init(struct c985_dev *dev);
void c985_v4l2_cleanup(struct c985_dev *dev);
int c985_v4l2_boot_encoder(struct c985_dev *dev);
void c985_v4l2_stop_encoder(struct c985_dev *dev);
void c985_v4l2_teardown(struct c985_dev *dev);
void c985_enc_write_config(struct c985_dev *dev, const u32 *cfg);

struct c985_stream_stats {
    atomic64_t frames_done;
    atomic64_t frames_dropped;
    atomic64_t bytes_done;
    atomic64_t desc_non40;
    atomic64_t desc_bad;
    atomic64_t no_buf;
};
void c985_v4l2_get_stats(struct c985_dev *dev, struct c985_stream_stats *s);
void c985_audio_get_stats(struct c985_dev *dev, struct c985_stream_stats *s);

/* ALSA audio capture device (Phase: audio) */
int c985_audio_init(struct c985_dev *dev);
void c985_audio_cleanup(struct c985_dev *dev);
int c985_audio_boot(struct c985_dev *dev);
void c985_audio_stop(struct c985_dev *dev);
void c985_audio_teardown(struct c985_dev *dev);

/* Inline register access */
static inline u32 c985_read_bar0(struct c985_dev *dev, u32 offset)
{
    return ioread32(dev->bar0 + offset);
}

static inline void c985_write_bar0(struct c985_dev *dev, u32 offset, u32 val)
{
    iowrite32(val, dev->bar0 + offset);
}

static inline u32 c985_read_bar1(struct c985_dev *dev, u32 offset)
{
    return ioread32(dev->bar1 + offset);
}

static inline void c985_write_bar1(struct c985_dev *dev, u32 offset, u32 val)
{
    iowrite32(val, dev->bar1 + offset);
}

#endif /* _C985_H_ */
