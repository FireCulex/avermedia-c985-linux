#!/bin/sh
set -eu

# Almost every step here (insmod, sysfs writes, rmmod) needs root. Without it
# insmod fails with EPERM, and the error used to be swallowed by
# "2>/dev/null || true", which surfaced as the misleading
# "driver directory not found after module load". Fail loudly and up front.
if [ "$(id -u)" != "0" ]; then
    echo "error: must run as root; try: sudo $0 $*" >&2
    exit 1
fi

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

# Helper: rmmod with the real error surfaced, under a hard timeout.
# A holder wedged in D state cannot be reaped, so never let rmmod block here.
unload_module() {
    lsmod | grep -q "^c985 " || return 0
    if out=$(timeout 10 rmmod c985 2>&1); then
        return 0
    fi
    echo "${1:-unload}: rmmod c985 failed: ${out:-<no output>}" >&2
    return 1
}

# Helper: insmod with the real error surfaced. A swallowed EPERM /
# "Unknown symbol" / "Invalid module format" used to look like a missing
# driver directory instead of a module that never loaded.
load_module() {
    if out=$(insmod "$OUR_KO" $DEBUG 2>&1) && lsmod | grep -q "^c985 "; then
        return 0
    fi
    echo "bind: insmod $OUR_KO ${DEBUG:-}failed: ${out:-<no output>}" >&2
    echo "bind: check 'sudo dmesg -T | tail' for the kernel-side reason" >&2
    return 1
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

    # NOTE on /sys/module/c985/refcnt: a *healthy* probed c985 always reads 1,
    # not 0. snd_card_new(..., THIS_MODULE) takes a module reference for the
    # "c985audio" card, and because probe() runs inside init_module that
    # reference lands in the module's init_refcnt, which the kernel's
    # try_release_all_refs() deliberately does not treat as a blocker. So
    # refcnt != 0 is the normal steady state, NOT evidence of a wedged holder.
    # Treating it as one made every rebind of a working driver fail with a
    # bogus "reboot required" and made the srcversion-reload path below
    # unreachable. Real proof of a problem is rmmod itself failing.

    # Load videobuf2/v4l2 dependencies (in-kernel driver is a v4l2 capture
    # device now; insmod cannot resolve these symbol deps by itself)
    for m in videobuf2-common videobuf2-memops videobuf2-dma-sg videobuf2-v4l2; do
        if ! lsmod | grep -q "^$m "; then
            if ! modprobe "$m" 2>/tmp/c985-modprobe-$$.err; then
                echo "bind: modprobe $m failed: $(cat /tmp/c985-modprobe-$$.err)" >&2
                rm -f /tmp/c985-modprobe-$$.err
                exit 1
            fi
            rm -f /tmp/c985-modprobe-$$.err
        fi
    done

    # If module loaded but device not bound, unload first (failed probe leaves module)
    if lsmod | grep -q "^c985 "; then
        if [ ! -L "$DRV/$PCI_ID" ]; then
            echo "bind: c985 loaded but $PCI_ID not bound; unloading stale module" >&2
            kill_video_holders
            kill_alsa_holders
            unload_module "bind" || true
            sleep 0.2
            rmmod -f c985 2>/dev/null || true
        fi
    fi

    # Load module if not loaded; if stale (srcversion mismatch), force-reload.
    # insmod triggers probe(); the module remains loaded even if probe fails,
    # so a separate $DRV/bind write would trigger a SECOND probe attempt. We
    # must load exactly once and check the result, never double-bind.
    if ! lsmod | grep -q "^c985 "; then
        load_module || exit 1
    else
        disk_sv=$(modinfo "$OUR_KO" -F srcversion 2>/dev/null)
        live_sv=$(cat /sys/module/c985/srcversion 2>/dev/null)
        if [ -n "$disk_sv" ] && [ -n "$live_sv" ] && [ "$disk_sv" != "$live_sv" ]; then
            echo "bind: loaded c985 (src $live_sv) differs from $OUR_KO (src $disk_sv); reloading" >&2
            kill_video_holders
            kill_alsa_holders
            unload_module "bind" || true
            sleep 0.2
            rmmod -f c985 2>/dev/null || true
            load_module || exit 1
        fi
    fi

    # Wait for driver directory to appear (up to 5 seconds)
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -d "$DRV" ] && break
        sleep 0.5
    done

    if [ ! -d "$DRV" ]; then
        if lsmod | grep -q "^c985 "; then
            # Module is in, so the .ko loaded fine; the failure is in probe().
            echo "bind: c985 loaded but probe() did not register a PCI driver ($DRV absent); see dmesg" >&2
        else
            echo "bind: c985 is not loaded; $OUR_KO failed to insert" >&2
        fi
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
        unload_module "bind" || true
        exit 1
    fi
    ;;
  unbind)
    kill_video_holders
    kill_alsa_holders

    if [ -L "$DRV/$PCI_ID" ]; then
        swrite "$DRV/unbind" "$PCI_ID"
    fi
    # Retry rmmod briefly; holders die asynchronously. refcnt is NOT the
    # success criterion here: probe() leaves a benign init_refcnt=1 behind for
    # the ALSA card, so check `lsmod`, not /sys/module/c985/refcnt.
    for i in 1 2 3 4 5; do
        lsmod | grep -q "^c985 " || break
        unload_module "unbind" >/dev/null 2>&1 || true
        sleep 0.2
    done
    if lsmod | grep -q "^c985 "; then
        unload_module "unbind" || true
    fi
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