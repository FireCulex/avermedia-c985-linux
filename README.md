# AVerMedia C985 Linux Driver

Linux V4L2 driver for the AVerMedia C985 (1af2:a001), implementing support for the card's vendor-specific mailbox-based firmware protocol, reverse-engineered from the Windows driver and ARM firmware.

**Video capture: working** | **Audio capture: working (raw LPCM via ALSA PCM)**

![OBS Capture](https://i.imgur.com/TRJMRWp.png)

## Hardware

- AVerMedia C985 (Live Gamer HD 2) PCIe capture card
- PCI Vendor ID: `0x1AF2`, Device ID: `0xA001`
- Onboard Nuvoton NUC100RD2BN microcontroller
- TI TLV320AIC3101 audio codec

## Requirements

- Linux kernel headers (matching running kernel)
- Clang/LLVM (`LLVM=1` required for build)
- Physical C985 hardware
- `sudo` for module bind/unbind
- Firmware files in `/lib/firmware/avermedia/`

## Quick Start

```bash
# 1. Build the kernel module
cd kernel/c985 && LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

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
LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
```

Output: `c985.ko`

Clean: `LLVM=1 make -C /lib/modules/$(uname -r)/build M=$(pwd) clean`

**Note:** `LLVM=1` (Clang) is required — GCC builds are not supported.

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

**What tests verify:**
- Device format: 1920×1080 YUV420 (YU12)
- 30 fps timing (33ms frame intervals ±15%)
- Monotonic sequence numbers (no drops/duplicates)
- Frame uniqueness (visual difference via Y-plane comparison)
- Effective FPS ≥ 15 (detects static/black frames)

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
- Requires Clang/LLVM for build
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