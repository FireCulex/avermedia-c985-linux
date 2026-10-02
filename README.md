# AVerMedia C985 Linux Driver

Linux V4L2 driver for the AVerMedia C985 (1af2:a001), implementing support for the card's vendor-specific mailbox-based firmware protocol, reverse-engineered from the Windows driver and ARM firmware.

![OBS Capture](https://i.imgur.com/TRJMRWp.png)

## Project Status

| Feature | State |
|---------|-------|
| Video capture | **Working** — 1920×1080 YUV420 (YU12) @ 30 fps via V4L2/vb2 |
| Audio capture | **Working** — raw LPCM S16_LE interleaved stereo @ 48 kHz via ALSA PCM |
| Other AVerMedia models | **Not supported** |

Honest notes before you start:

- This is an **unofficial, reverse-engineered** driver. The firmware protocol was
  recovered from the Windows driver and the ARM firmware blobs; there is no vendor
  documentation or support.
- Audio only becomes available **after the video device is opened for streaming**
  (see [Audio Capture](#audio-capture)).
- Frame capture is **not bit-perfect** — expect the occasional duplicate or dropped
  frame. This is a property of the card's firmware/encoder and appears on Windows
  too; see [Frame-Capture Quality Baseline](#frame-capture-quality-baseline).
- `make` must use the **same compiler family** as your kernel (gcc or clang). This
  is detected automatically; see [Building](#building).

## Contents

- [Project Status](#project-status)
- [Hardware](#hardware)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [Firmware Installation](#firmware-installation)
- [Building](#building)
- [Binding / Unbinding](#binding--unbinding)
- [Audio Capture](#audio-capture)
- [Running Tests](#running-tests)
- [Debugfs Interface](#debugfs-interface)
- [Architecture](#architecture)
- [Known Limitations](#known-limitations)
  - [Frame-Capture Quality Baseline](#frame-capture-quality-baseline)
- [Common Gotchas](#common-gotchas)
- [Development](#development)
- [License](#license)
- [Contributing](#contributing)

## Hardware

- AVerMedia C985 (Live Gamer HD 2) PCIe capture card
- PCI Vendor ID: `0x1AF2`, Device ID: `0xA001`
- Onboard Nuvoton NUC100RD2BN microcontroller
- TI TLV320AIC3101 audio codec

## Requirements

- Linux kernel headers (matching running kernel)
- The same compiler family the running kernel was built with (gcc or clang — see [Building](#building))
- Physical C985 hardware
- `sudo` for module bind/unbind
- Firmware files in `/lib/firmware/avermedia/`

## Quick Start

```bash
# 1. Build the kernel module
cd kernel/c985 && make

# 2. Install firmware (see Firmware section below)

# 3. Bind the driver (requires sudo)
sudo ./tools/c985-bind.sh bind

# 4. Run tests (requires sudo + physical device)
./run_tests.sh
```

## Firmware Installation

The driver loads two firmware files via `request_firmware()`. Place them in `/lib/firmware/avermedia/`:

```
/lib/firmware/avermedia/
├── qpvidfwpcie.bin   # Video firmware (~353 KB)
├── qpaudfw.bin       # Audio firmware (~363 KB)
```

Firmware can be extracted from the [official AVerMedia Windows driver](https://www.avermedia.com/uk/support/download?model_number=C985).

## Building

```bash
cd kernel/c985
make          # build for the running kernel
```

Output: `c985.ko`. Clean with `make clean`.

Common options (full list: `make help`):

```bash
make KVER=7.2.7-1-cachyos     # build against a specific installed kernel
make KBUILD_FLAGS='W=1'       # verbose build
make CC_FAMILY=clang          # override compiler auto-detection
make help                     # list targets and variables
```

**Compiler family matters.** An out-of-tree module must be built with the *same
compiler family* as the target kernel, because kbuild bakes the kernel's own
toolchain flags into `KBUILD_CFLAGS` (e.g. `-mpreferred-stack-boundary=3` for
gcc, `-mstack-alignment=8` for clang) and the wrong compiler rejects them. The
kernel build tree injects the correct flags but does **not** select the compiler
for you, so this Makefile detects the family and passes `LLVM=1` only when it is
actually needed. A plain `make` targets the running kernel (`uname -r`) and
therefore always matches it. Every build prints the resolved toolchain:

```
  KVER     7.2.7-1-cachyos
  KDIR     /lib/modules/7.2.7-1-cachyos/build
  CC       clang
```

The detection order and the full `$(LLVM)` / `CONFIG_CC_IS_*` rationale live in
the comments at the top of `kernel/c985/Makefile` and in `make help`.

## Binding / Unbinding

Use the helper script `tools/c985-bind.sh` (requires root):

```bash
# Bind device (loads module, adds PCI ID, binds)
sudo ./tools/c985-bind.sh bind

# Unbind device (kills /dev/video* holders, unbinds, rmmod)
sudo ./tools/c985-bind.sh unbind

# Check bind status
sudo ./tools/c985-bind.sh status

# Debug mode (enables driver debug prints)
sudo ./tools/c985-bind.sh --debug bind
```

The script handles:
- Auto-detecting the C985 PCI device
- Loading `videobuf2-*` dependencies
- Detecting stale modules via `srcversion` mismatch and force-reloading
- Killing processes holding stale `/dev/video*` fds

Override kernel module path: `C985_KO=/path/to/c985.ko sudo ./tools/c985-bind.sh bind`

Override PCI ID: `C985_PCI_ID=0000:01:00.0 sudo ./tools/c985-bind.sh bind`

## Audio Capture

The driver registers an ALSA capture device (`c985-audio`) delivering raw LPCM S16_LE interleaved stereo (48 kHz). Userspace consumes it as ordinary PCM.

```bash
# List audio devices
arecord -l

# Capture raw PCM to file (card index varies, check arecord -l)
arecord -D hw:C985 -f S16_LE -c 2 -r 48000 -t raw audio.raw

# Play back with ffmpeg (decode PCM on the fly)
ffmpeg -f alsa -i hw:C985 -f s16le - | ffplay -f s16le -

# Or save as WAV and play
ffmpeg -f alsa -i hw:C985 -c copy audio.wav
ffplay audio.wav
```

**Note:** The ALSA device appears after `c985_audio_boot()` runs (triggered by opening the video device for streaming). Open `/dev/video0` first (e.g., `v4l2-ctl --stream-mmap=3 --stream-to=/dev/null`), then the audio device will be active.

## Running Tests

```bash
# Full test suite (binds module, captures 30 frames)
./run_tests.sh

# Single test
./run_tests.sh tests/test_capture.py::test_format_verification

# Override device path
C985_DEVICE=/dev/video2 ./run_tests.sh
```

**Test requirements:**
- Physical C985 hardware at `/dev/video0` (or `C985_DEVICE` env)
- `sudo` for module bind/unbind
- Python venv auto-created at `.venv/` with `pytest`
- Kernel module at `kernel/c985/c985.ko` must exist (build first)
- **A moving signal on the capture input** (see [Input signal](#input-signal))

**Input signal.** These are capture-quality tests, so the source must be moving.
`test_capture.py`'s frame-uniqueness and effective-FPS checks fail on a static or
black input, so feed the card something in motion. For the A/V sync test
specifically, play the project's sync-test leader — an A/V sync test video with a
beep + on-screen toggle every second, panned right → center → left → center:

  <https://www.youtube.com/watch?v=QzomK1fdSUg>

**What tests verify:**
- Device format: 1920×1080 YUV420 (YU12)
- 30 fps timing (33ms frame intervals ±15%)
- Monotonic sequence numbers (no drops/duplicates)
- Frame uniqueness (visual difference via Y-plane comparison)
- Effective FPS ≥ 15 (detects static/black frames)

**`test_sync.py`** is different: it does **not** touch the capture device. It
analyzes an already-recorded container of the leader above. There is no default
path — point it at your recording with `SYNC_TEST_VIDEO`, or the test is skipped:

```bash
SYNC_TEST_VIDEO=/path/to/capture.mkv pytest tests/test_sync.py -s
```

`./run_tests.sh` runs the whole `tests/` directory, so `test_sync.py` is skipped
there unless `SYNC_TEST_VIDEO` is set.

## Debugfs Interface

When loaded, the driver exposes debugfs at `/sys/kernel/debug/c985/`:

| File | Description |
|------|-------------|
| `regs` | BAR0/BAR1 register dump |
| `mbox_log` | Mailbox command/response log |
| `frame_read` | Capture a frame to userspace (debug) |
| `cpr_peek` | Read CPR (Card Program Register) registers |
| `dma_status` | DMA channel state |

## Architecture

```
PCIe → BAR0 (DMA) / BAR1 (Mailbox/ARM) → Firmware (QPSOS) → v4l2 → /dev/video0
```

**Kernel module** (`kernel/c985/`, 10 source files):

| File | Purpose |
|------|---------|
| `c985_main.c` | Probe/remove, core init |
| `c985_v4l2.c` | V4L2/VB2 streaming interface |
| `c985_dma.c` | PL330-style DMA engine, frame-mode reads (contiguous + scatter-gather) |
| `c985_mbox.c` | Mailbox command/response + interrupt-driven frame FIFO |
| `c985_irq.c` | MSI/MSI-X, PCIe/HCI/doorbell interrupt handling |
| `c985_fw.c` | QPSOS firmware load (video + audio) |
| `c985_audio.c` | ALSA PCM capture (raw LPCM) |
| `c985_nuc100.c` | NUC100 MCU register access |
| `c985_debugfs.c` | Debugfs knobs |
| `cpr.c` | CPR register helpers |

**References:**
- [Nuvoton NUC100RD2BN](https://www.keil.com/dd/chip/7119.htm)
- [TI TLV320AIC3101 Datasheet](https://www.ti.com/lit/ds/symlink/tlv320aic3101.pdf)

## Known Limitations

- No support for other AVerMedia models
- Build requires the same compiler family as the target kernel (gcc or clang, auto-selected)
- Audio capture exposes raw LPCM; userspace consumes it as ordinary PCM (e.g., ffmpeg `-f alsa -i hw:X -f s16le -`)

Video capture uses `vb2_dma_sg` (scatter-gather) with a 64-bit DMA mask. Each Y/U/V plane read walks the `sg_table` emitting one chained descriptor per SG element (`c985_dma_read_frame_mode_sg`). Buffer count is a hard 4 (firmware 4-slot ring).

### Frame-Capture Quality Baseline

Perfect frame capture is unlikely on this card, even in Windows — confirmed both by `test_sync.py` and by visual inspection in Avidemux, where the on-screen leader counter visibly skips `29→02` (missing `00/01`).

- Windows 2-minute baseline (`windows_2min_sync_test.mp4`): 16 duplicate frames + 3 dropped, clustering roughly every 15.5s.
- Linux driver (30s captures): ~1–3 duplicates, 0 drops, on the same ~15.5s cadence.
- Observed error rate: 0–0.5% of frames (duplicates + skips) per 2-minute capture, on both platforms.

Do not treat "zero dup / zero drop" as a driver correctness target. Both platforms show the same underlying firmware/encoder ~15.5s cadence (likely GOP/IDR boundary or ring-buffer recycle).

## Common Gotchas

| Issue | Cause | Fix |
|-------|-------|-----|
| `rmmod c985` fails "Module in use" | Process holds `/dev/video*` | `tools/c985-bind.sh unbind` kills holders |
| Stale module after rebuild | `srcversion` mismatch | Bind script auto-detects and force-reloads |
| Firmware not found | Files missing from `/lib/firmware/avermedia/` | Install `qpvidfwpcie.bin` + `qpaudfw.bin` |
| No device detected | Wrong PCI ID or card not seated | Verify `lspci -d 1af2:a001` |
| Tests fail | Module not bound or no hardware | Run `sudo ./tools/c985-bind.sh bind` first |

## Development

Developed with the help of [`int`](https://github.com/FireCulex/int), an MCP tool providing persistent memory across sessions (RAG-like memory for agents).

## License

GPL-2.0

## Contributing

Kernel coding style. No CI/CD, no static analysis configured. Test on hardware before submitting.