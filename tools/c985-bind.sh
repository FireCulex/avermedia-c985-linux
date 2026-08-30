#!/bin/sh
set -eu

VENDOR="0x1AF2"
DEVICE="0xA001"
DRV=/sys/bus/pci/drivers/c985
DEBUG=""

# Auto-detect PCI ID for the AVerMedia C985 device
detect_pci_id() {
    for dev in /sys/bus/pci/devices/*; do
        ven=$(cat "$dev/vendor" 2>/dev/null || true)
        dev_id=$(cat "$dev/device" 2>/dev/null || true)
        if [ "${ven^^}" = "${VENDOR^^}" ] && [ "${dev_id^^}" = "${DEVICE^^}" ]; then
            basename "$dev"
            return 0
        fi
    done
    return 1
}

PCI_ID="${C985_PCI_ID:-$(detect_pci_id)}"
if [ -z "$PCI_ID" ]; then
    echo "error: could not auto-detect C985 device; set C985_PCI_ID env var" >&2
    exit 1
fi

# Default KO path relative to script location; override with C985_KO
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUR_KO="${C985_KO:-$SCRIPT_DIR/../kernel/c985/c985.ko}"

# Helper: write to sysfs (requires root - run script with sudo)
swrite() {
    local file="$1"
    local content="$2"
    echo "$content" > "$file" 2>/dev/null || true
}

# Kill stragglers holding a stale /dev/video* fd: a dead fd pins the
# module refcount and makes rmmod fail with "Module c985 is in use",
# which then makes the NEXT bind skip insmod and reuse the stale .ko.
kill_video_holders() {
    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        for fd in /proc/$pid/fd/*; do
            case "$(readlink "$fd" 2>/dev/null)" in
                */dev/video*)
                    echo "unbind: killing PID $pid ($(cat /proc/$pid/comm 2>/dev/null)) holding $fd" >&2
                    kill "$pid" 2>/dev/null || true
                    break
                    ;;
            esac
        done
    done
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --debug)
            DEBUG="debug=1"
            shift
            ;;
        *)
            break
            ;;
    esac
done

case "${1:-}" in
  bind)
    # The c985 module IS a v4l2 capture device now; no loopback needed.

    # Load videobuf2/v4l2 dependencies (in-kernel driver is a v4l2 capture
    # device now; insmod cannot resolve these symbol deps by itself)
    for m in videobuf2-common videobuf2-memops videobuf2-dma-contig videobuf2-v4l2; do
        if ! lsmod | grep -q "^$m "; then
            modprobe "$m" 2>/dev/null || true
        fi
    done

    # Load module if not loaded; if stale (srcversion mismatch), force-reload
    if ! lsmod | grep -q "^c985 "; then
        insmod "$OUR_KO" $DEBUG 2>/dev/null || true
    else
        disk_sv=$(modinfo "$OUR_KO" -F srcversion 2>/dev/null)
        live_sv=$(cat /sys/module/c985/srcversion 2>/dev/null)
        if [ -n "$disk_sv" ] && [ -n "$live_sv" ] && [ "$disk_sv" != "$live_sv" ]; then
            echo "bind: loaded c985 (src $live_sv) differs from $OUR_KO (src $disk_sv); removing" >&2
            rmmod c985 2>/dev/null || true
            sleep 0.2
            rmmod -f c985 2>/dev/null || true
            insmod "$OUR_KO" $DEBUG 2>/dev/null || true
        fi
    fi
    
    # Wait for driver directory to appear (up to 5 seconds)
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -d "$DRV" ] && break
        sleep 0.5
    done
    
    if [ ! -d "$DRV" ]; then
        echo "driver directory not found after module load" >&2
        exit 1
    fi
    
    # Add PCI ID to driver (ignore "File exists")
    swrite "$DRV/new_id" "$VENDOR $DEVICE"
    
    # Bind device
    swrite "$DRV/bind" "$PCI_ID"
    
    # Verify
    if [ -L "$DRV/$PCI_ID" ]; then
        echo "bound: $(readlink "$DRV/$PCI_ID")"
    else
        echo "failed to bind" >&2
        exit 1
    fi
    ;;
  unbind)
    kill_video_holders

    if [ -L "$DRV/$PCI_ID" ]; then
        swrite "$DRV/unbind" "$PCI_ID"
    fi
    # Retry rmmod briefly; refcount drops after unbind + stragglers die.
    for i in 1 2 3 4 5; do
        rmmod c985 2>/dev/null && break
        sleep 0.2
    done
    rmmod v4l2loopback 2>/dev/null || true
    ;;
  status)
    if [ -L "$DRV/$PCI_ID" ]; then
        echo "bound: $(readlink "$DRV/$PCI_ID")"; exit 0
    fi
    echo "unbound" >&2; exit 1
    ;;
  *)
    echo "usage: $0 [--debug] bind|unbind|status" >&2; exit 2
    ;;
esac