# AVerMedia C985 Linux Driver — Agent Instructions

## Repository Overview
Kernel driver for AVerMedia C985 PCIe capture card (1af2:a001). Implements v4l2 video capture + ALSA audio capture via custom DMA/mailbox firmware protocol. **Video capture: working. Audio capture: working (AAC-LC passthrough via ALSA PCM).**

## Build & Test Commands
```bash
# Build kernel module (requires kernel headers, LLVM/Clang)
cd kernel/c985 && LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
# or: cd kernel/c985 && make        # (uses Makefile's LLVM=1 default)

# Clean
cd kernel/c985 && LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) clean

# Run tests (requires physical device + sudo)
./run_tests.sh                              # Full suite: binds module, captures 30 frames
./run_tests.sh tests/test_capture.py::test_format_verification  # Single test
C985_DEVICE=/dev/video2 ./run_tests.sh      # Override device path
```

## Test Requirements
- Physical C985 hardware at `/dev/video0` (or `C985_DEVICE` env)
- `sudo` for module bind/unbind via `tools/c985-bind.sh`
- Python venv auto-created at `.venv/` with `pytest` (see `requirements.txt`)
- Kernel module at `kernel/c985/c985.ko` must exist (build first)
- Firmware in `/lib/firmware/avermedia/`: `qpvidfwpcie.bin` + `qpaudfw.bin`

## Architecture Notes
- **Kernel module**: `kernel/c985/` — 10 source files, builds to `c985.ko`
  - `c985_main.c` — probe/remove, core init
  - `c985_v4l2.c` — v4l2/vb2 streaming interface
  - `c985_dma.c` — PL330-style DMA engine, frame-mode reads (contiguous + scatter-gather)
  - `c985_mbox.c` — Mailbox command/response + interrupt-driven frame FIFO
  - `c985_irq.c` — MSI/MSI-X, PCIe/HCI/doorbell interrupt handling
  - `c985_fw.c` — QPSOS firmware load (video + audio)
  - `c985_audio.c` — ALSA PCM capture (AAC-LC passthrough, SNDRV_PCM_FORMAT_MPEG via S16_LE opaque transport)
  - `c985_nuc100.c` — CPR register access for NUC100 sensor config
  - `c985_debugfs.c` — Debugfs knobs (frame_read, CPR peek, diags)
  - `cpr.c` — CPR register helpers
- **Bind script**: `tools/c985-bind.sh` — handles PCI ID binding, module reload on srcversion mismatch, kills stale `/dev/video*` and ALSA holders
- **Test**: `tests/test_capture.py` — Uses `v4l2-ctl --stream-mmap --stream-show-delta-now` to verify 30fps timing, monotonic sequences, YUV420 1920x1080, frame uniqueness

## Key Conventions
- Kernel build uses `LLVM=1` (Clang) — required; GCC builds not supported
- Firmware loaded via kernel `request_firmware()` — expects files in `/lib/firmware/avermedia/`
- Debugfs at `/sys/kernel/debug/c985/` exposes: `regs`, `mbox_log`, `frame_read`, `cpr_peek`, `dma_status`
- Module dependencies: `videobuf2-common`, `videobuf2-memops`, `videobuf2-dma-sg`, `videobuf2-v4l2` (loaded by bind script)

## Sudo / Privilege Rules (MANDATORY — DO NOT VIOLATE)
- FIRST ACTION whenever a step may need privilege: run `sudo -l` (NEVER `sudo -n true`, NEVER guess).
- Passwordless (NOPASSWD) commands available (from `sudo -l`, re-read it every session):
  - `tools/c985-bind.sh`
  - `dmesg -T`
  - `ls -la /sys/kernel/debug/c985/*`, `cat /sys/kernel/debug/c985/*`, `tee /sys/kernel/debug/c985/*`, `ls /sys/kernel/debug/c985/`
- `(ALL) ALL` requires an interactive password — CANNOT be run non-interactively. **If a step needs `sudo` for something NOT in the NOPASSWD list, STOP and ask the user for a sudoers line. Do NOT attempt a sudo command that will prompt for a password — it hangs and wastes the user's time.**
- Runtime debug output WITHOUT sudo:
  - `/sys/kernel/debug/dynamic_debug/control` is directly writable by the user. Use it to enable kernel `dev_dbg`/`pr_debug` at runtime:
    - `echo "module c985 +p" > /sys/kernel/debug/dynamic_debug/control`
    - Verify writability first: `echo -n > /sys/kernel/debug/dynamic_debug/control`
  - Module built with `ccflags-y += -DDEBUG` always emits `dev_dbg` without dynamic_debug; prefer runtime `+p` to avoid rebuild.

## Common Gotchas
- `rmmod c985` fails "Module in use" if any process holds `/dev/video*` or ALSA devices — bind script kills holders
- Stale module (rebuild without reload) detected via `srcversion` mismatch — bind script force-reloads
- Tests require bound module + running firmware; `run_tests.sh` handles full bind/test cycle
- No CI/CD, no static analysis, no formatting tools configured
- If bind script reports "c985 is loaded with refcnt=X (in use / deadlocked); reboot required" — reboot is the only fix
- Audio capture implemented as ALSA PCM (AAC-LC passthrough); userspace must decode raw AAC frames
- Video capture uses `vb2_dma_sg` (scatter-gather) with a 64-bit DMA mask. Each Y/U/V plane read walks the `sg_table` emitting one chained descriptor per SG element (`c985_dma_read_frame_mode_sg`). Buffer count is a hard 4 (firmware 4-slot ring).