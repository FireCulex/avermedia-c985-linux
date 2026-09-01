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

# Run systemctl --user as the actual user (not root)
# SUDO_USER is set when script is run via sudo.
# Must propagate XDG_RUNTIME_DIR and DBUS_SESSION_BUS_ADDRESS so the root
# shell can reach the target user's systemd session bus (otherwise
# systemctl --user silently does nothing and audio never comes back).
user_systemctl() {
    if [ -n "${SUDO_USER:-}" ]; then
        sudo -u "$SUDO_USER" \
            XDG_RUNTIME_DIR="/run/user/$(id -u "$SUDO_USER")" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$SUDO_USER")/bus" \
            systemctl --user "$@" 2>/dev/null || true
    else
        systemctl --user "$@" 2>/dev/null || true
    fi
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

# Kill stragglers holding ALSA devices for the c985 audio card (control/PCM).
# WirePlumber/PipeWire holds /dev/snd/controlC* and /dev/snd/pcmC*D*c open,
# which keeps the c985 module refcount nonzero via snd_pcm dependency.
kill_alsa_holders() {
    # Find c985 ALSA card index from /proc/asound/cards
    card_idx=$(awk '/c985/ { gsub(/\[|\]/, "", $1); print $1 }' /proc/asound/cards 2>/dev/null | head -1)
    [ -z "$card_idx" ] && return 0

    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        for fd in /proc/$pid/fd/*; do
            target=$(readlink "$fd" 2>/dev/null || true)
            case "$target" in
                /dev/snd/controlC"$card_idx"|/dev/snd/pcmC"$card_idx"D*c)
                    echo "unbind: killing PID $pid ($(cat /proc/$pid/comm 2>/dev/null)) holding $target" >&2
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

    # Refuse to proceed if a live module is wedged (nonzero refcount from
    # D-state holders that a pkill can't reap). rmmod would hang forever.
    if lsmod | grep -q "^c985 "; then
        live_refcnt=$(cat /sys/module/c985/refcnt 2>/dev/null || echo 0)
        if [ "$live_refcnt" != "0" ]; then
            echo "bind: c985 is loaded with refcnt=$live_refcnt (in use / deadlocked); reboot required." >&2
            exit 1
        fi
    fi

    # Load videobuf2/v4l2 dependencies (in-kernel driver is a v4l2 capture
    # device now; insmod cannot resolve these symbol deps by itself)
    for m in videobuf2-common videobuf2-memops videobuf2-dma-contig videobuf2-v4l2; do
        if ! lsmod | grep -q "^$m "; then
            modprobe "$m" 2>/dev/null || true
        fi
    done

    # If module loaded but device not bound, unload first (failed probe leaves module)
    if lsmod | grep -q "^c985 "; then
        if [ ! -L "$DRV/$PCI_ID" ]; then
            echo "bind: c985 loaded but $PCI_ID not bound; unloading stale module" >&2
            rmmod c985 2>/dev/null || true
            sleep 0.2
            rmmod -f c985 2>/dev/null || true
        fi
    fi

    # Load module if not loaded; if stale (srcversion mismatch), force-reload.
    # insmod triggers probe(); the module remains loaded even if probe fails,
    # so a separate $DRV/bind write would trigger a SECOND probe attempt. We
    # must load exactly once and check the result, never double-bind.
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

    # insmod's probe should have already claimed the device via the module's
    # PCI ID table. Only bind manually if the device still isn't bound and the
    # module was loaded without a matching ID table entry.
    if [ -L "$DRV/$PCI_ID" ]; then
        echo "bound: $(readlink "$DRV/$PCI_ID")"
    elif [ -f "$DRV/bind" ]; then
        swrite "$DRV/new_id" "$VENDOR $DEVICE"
        swrite "$DRV/bind" "$PCI_ID"
        if [ -L "$DRV/$PCI_ID" ]; then
            echo "bound: $(readlink "$DRV/$PCI_ID")"
        else
            echo "failed to bind" >&2
            exit 1
        fi
    else
        echo "failed to bind: driver has no bind interface (probe failed)" >&2
        rmmod c985 2>/dev/null || true
        exit 1
    fi
    ;;
  unbind)
    kill_video_holders
    kill_alsa_holders

    if [ "$(cat /sys/module/c985/refcnt 2>/dev/null || echo 0)" != "0" ]; then
        echo "unbind: c985 has nonzero refcount (live users present); refusing to force-unload. Kill holders / reboot." >&2
        exit 1
    fi

    if [ -L "$DRV/$PCI_ID" ]; then
        swrite "$DRV/unbind" "$PCI_ID"
    fi
    # Retry rmmod briefly; refcount drops after unbind + stragglers die.
    for i in 1 2 3 4 5; do
        rmmod c985 2>/dev/null && break
        sleep 0.2
    done
    rmmod v4l2loopback 2>/dev/null || true

    # Restart user PipeWire/WirePlumber stack so audio works again.
    # WirePlumber is a session manager layered on top of pipewire, so bring
    # pipewire up first and let wireplumber follow (avoids WP exiting because
    # its pipewire dependency wasn't ready yet).
    # stop + start handles both the "killed" and "still active" cases
    # (restart fails if the unit is already dead, start fails if it's active).
    for svc in pipewire pipewire-pulse wireplumber; do
        user_systemctl stop "$svc"
        sleep 0.3
        user_systemctl start "$svc"
    done

    # Wait for wireplumber to be fully active (max 5 seconds)
    for i in 1 2 3 4 5; do
        user_systemctl is-active --quiet wireplumber && break
        sleep 1
    done
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