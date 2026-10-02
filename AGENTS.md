# AVerMedia C985 Linux Driver — Agent Instructions

## Repository Overview
Kernel driver for AVerMedia C985 PCIe capture card (1af2:a001). Implements v4l2 video capture + ALSA audio capture via custom DMA/mailbox firmware protocol. **Video capture: working. Audio capture: working (raw LPCM via ALSA PCM).**

## Build & Test Commands
```bash
# Build kernel module (requires kernel headers)
cd kernel/c985 && make                    # auto-selects the target kernel's compiler

# Build against a specific installed kernel
cd kernel/c985 && make KVER=7.2.7-1-cachyos

# Verbose build / override compiler detection
cd kernel/c985 && make KBUILD_FLAGS='W=1'
cd kernel/c985 && make CC_FAMILY=clang         # force clang
cd kernel/c985 && make help                    # list targets and variables

# Clean
cd kernel/c985 && make clean

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
  - `c985_audio.c` — ALSA PCM capture (raw LPCM S16_LE stereo @48kHz)
  - `c985_nuc100.c` — NUC100 MCU register access via GPIO bit-bang I2C
  - `c985_debugfs.c` — Debugfs knobs (frame_read, CPR peek, diags)
  - `cpr.c` — CPR register helpers
- **Bind script**: `tools/c985-bind.sh` — handles PCI ID binding, module reload on srcversion mismatch, kills stale `/dev/video*` and ALSA holders
- **Test**: `tests/test_capture.py` — Uses `v4l2-ctl --stream-mmap --stream-show-delta-now` to verify 30fps timing, monotonic sequences, YUV420 1920x1080, frame uniqueness

## Key Conventions
- Kernel build is **compiler-agnostic**: `kernel/c985/Makefile` auto-detects the target kernel's compiler family and passes `LLVM=1` only when needed (`KVER` defaults to `uname -r`, so a plain `make` always matches the running kernel). Both gcc and clang kernels work with no extra flags. NEVER hardcode `LLVM=1` — an out-of-tree module must use the same compiler family as the target kernel, because kbuild bakes the kernel's toolchain into `KBUILD_CFLAGS`. **Critically, the kernel build tree does NOT auto-select the compiler**: its Makefile keys `CC=clang` on `$(LLVM)` being non-empty and otherwise uses `$(CROSS_COMPILE)gcc`, even when `CONFIG_CC_IS_CLANG=y` is in `auto.conf`. So a clang kernel's build dir hands you clang-only flags (`-mstack-alignment=8`, `-mretpoline-external-thunk`) and then invokes gcc — that is the failure this Makefile exists to prevent. Detection order: `CC_FAMILY` → `CONFIG_CC_IS_{CLANG,GCC}` in `$(KDIR)/include/config/auto.conf` → `LINUX_COMPILER` in `$(KDIR)/include/generated/compile.h` → fall back to gcc with a warning. CachyOS ships a mix: `*-cachyos` is clang/LLD, `*-cachyos-lts` and `*-arch1-1` are gcc.
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

## Frame-Capture Quality Baseline (IMPORTANT)
- **Perfect frame capture is unlikely on this card, even in Windows** — confirmed both by `test_sync.py` and by visual inspection in Avidemux, where the on-screen leader counter visibly skips `29→02` (missing `00/01`).
- Windows 2-minute baseline (`windows_2min_sync_test.mp4`): 16 duplicate frames + 3 dropped, clustering roughly every 15.5s. Linux driver (30s captures): ~1–3 duplicates, 0 drops, on the same ~15.5s cadence.
- Observed error rate: 0–0.5% of frames (duplicates + skips) per 2-minute capture, on both platforms.
- Do NOT treat "zero dup / zero drop" as a driver correctness target. Both platforms show the same underlying firmware/encoder ~15.5s cadence (likely GOP/IDR boundary or ring-buffer recycle).

## Common Gotchas
- `rmmod c985` fails "Module in use" if any process holds `/dev/video*` or ALSA devices — bind script kills holders
- Stale module (rebuild without reload) detected via `srcversion` mismatch — bind script force-reloads
- Tests require bound module + running firmware; `run_tests.sh` handles full bind/test cycle
- No CI/CD, no static analysis, no formatting tools configured
- **Never treat `/sys/module/c985/refcnt != 0` as a wedged module.** A healthy probed c985 always reads `1`: `snd_card_new(..., THIS_MODULE)` takes a module reference for the `c985audio` card, and because that happens inside `probe()` (i.e. inside `init_module`) it is counted in the module's `init_refcnt`, which the kernel's `try_release_all_refs()` intentionally does *not* treat as an unload blocker. The bind script used to refuse to rebind on that basis and print a bogus "in use / deadlocked; reboot required" — which also made its own `srcversion`-mismatch reload path unreachable, so `unbind` and every rebuild-rebind needed a reboot. Proof of a real problem is `rmmod` failing (now surfaced verbatim, under a 10 s timeout), not the refcnt value.
- The bind script requires root and says so up front. Running it without `sudo` previously failed as `driver directory not found after module load`, because `insmod`'s `EPERM` was discarded by `2>/dev/null || true`. Always `sudo tools/c985-bind.sh bind`.
- Audio capture implemented as ALSA PCM (raw LPCM S16_LE stereo @48kHz); userspace consumes it as ordinary PCM
- Video capture uses `vb2_dma_sg` (scatter-gather) with a 64-bit DMA mask. Each Y/U/V plane read walks the `sg_table` emitting one chained descriptor per SG element (`c985_dma_read_frame_mode_sg`). Buffer count is a hard 4 (firmware 4-slot ring).