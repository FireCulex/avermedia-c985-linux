# AVerMedia C985 Linux Driver

Linux V4L2 driver for the AVerMedia C985 (1af2:a001), implementing support for the card's vendor-specific mailbox-based firmware protocol, reverse-engineered from the Windows driver and ARM firmware.

**Video capture: working** | **Audio capture: not working**

![OBS Capture](https://i.imgur.com/TRJMRWp.png)

## Hardware

- AVerMedia C985 (Live Gamer HD 2) PCIe capture card
- PCI Vendor ID: `0x1AF2`, Device ID: `0xA001`
- Onboard Nuvoton NUC100 microcontroller for sensor config
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
| `cpr_peek` | Read CPR (NUC100) registers |
| `dma_status` | DMA channel state |

## Architecture

```
PCIe → BAR0 (DMA) / BAR1 (Mailbox/ARM) → Firmware (QPSOS) → v4l2 → /dev/video0
```

**Kernel module** (`kernel/c985/`, 9 source files):

| File | Purpose |
|------|---------|
| `c985_main.c` | Probe/remove, core init |
| `c985_v4l2.c` | V4L2/VB2 streaming interface |
| `c985_dma.c` | PL330-style DMA engine, frame-mode reads |
| `c985_mbox.c` | Mailbox command/response + interrupt-driven frame FIFO |
| `c985_irq.c` | MSI/MSI-X, PCIe/HCI/doorbell interrupt handling |
| `c985_fw.c` | QPSOS firmware load (video + audio) |
| `c985_nuc100.c` | CPR register access for NUC100 sensor config |
| `c985_debugfs.c` | Debugfs knobs |
| `cpr.c` | CPR register helpers |

**References:**
- [Nuvoton NUC100 Series Manual](https://www.nuvoton-tech.com/pdf-71/nuc100rd1bn.pdf)
- [TI TLV320AIC3101 Datasheet](https://www.ti.com/product/TLV320AIC3101)

## Known Limitations

- **Audio capture not implemented** — firmware loads but no ALSA/V4L2 audio path
- No support for other AVerMedia models
- Requires Clang/LLVM for build

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