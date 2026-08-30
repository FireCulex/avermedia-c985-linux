# AVerMedia C985 Linux Driver — Agent Instructions

## Repository Overview
Kernel driver for AVerMedia C985 PCIe capture card. Implements v4l2 video capture via custom DMA/mailbox firmware protocol.

## Build & Test Commands
```bash
# Build kernel module (requires kernel headers)
cd kernel/c985 && LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# Run tests (requires physical device + sudo)
./run_tests.sh                    # Full test suite (binds module, captures 30 frames)
./run_tests.sh tests/test_capture.py::test_format_verification  # Single test
C985_DEVICE=/dev/video2 ./run_tests.sh  # Override device path
```

## Test Requirements
- Physical C985 hardware at `/dev/video0` (or `C985_DEVICE` env)
- `sudo` for module bind/unbind via `tools/c985-bind.sh`
- Python venv auto-created at `.venv/` with `pytest`
- Kernel module at `kernel/c985/c985.ko` must exist (build first)

## Architecture Notes
- **Kernel module**: `kernel/c985/` — 9 source files, builds to `c985.ko`
  - `c985_main.c` — probe/remove, core init
  - `c985_v4l2.c` — v4l2/vb2 streaming interface (Phase 3/4)
  - `c985_dma.c` — PL330-style DMA engine, frame-mode reads
  - `c985_mbox.c` — Mailbox command/response + interrupt-driven frame FIFO
  - `c985_irq.c` — MSI/MSI-X, PCIe/HCI/doorbell interrupt handling
  - `c985_fw.c` — QPSOS firmware load (video + audio)
  - `c985_nuc100.c` — CPR register access for NUC100 sensor config
  - `c985_debugfs.c` — Debugfs knobs (frame_read, CPR peek, diags)
- **Bind script**: `tools/c985-bind.sh` — handles PCI ID binding, module reload on srcversion mismatch, kills stale `/dev/video*` holders
- **Test**: `tests/test_capture.py` — Uses `v4l2-ctl --stream-mmap --stream-show-delta-now` to verify 30fps timing, monotonic sequences, YUV420 1920x1080, frame uniqueness

## Key Conventions
- Kernel build uses `LLVM=1` (Clang) — required for this codebase
- Firmware loaded from kernel `request_firmware()` — expects `avermedia/qpvidfwpcie.bin` + `avermedia/qpaudfw.bin` in `/lib/firmware/`
- Debugfs at `/sys/kernel/debug/c985/` exposes: `regs`, `mbox_log`, `frame_read`, `cpr_peek`, `dma_status`

## Common Gotchas
- `rmmod c985` fails with "Module in use" if any process holds `/dev/video*` — bind script kills holders
- Stale module (rebuild without reload) detected via `srcversion` mismatch — bind script force-reloads
- Tests require bound module + running firmware; `run_tests.sh` handles full cycle
- No CI/CD, no static analysis, no formatting tools configured