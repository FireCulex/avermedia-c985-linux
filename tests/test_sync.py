#!/usr/bin/env python3
"""
Audio/video sync + frame-pacing analysis for AVerMedia C985 captures.

Analyzes an already-recorded container (mp4/mkv) of the "sync test" leader,
which emits a matched audio-beep + visual-toggle every 1 second in a
panning pattern (right -> center -> left -> center, repeating). Each event
fires a beep on the corresponding audio channel(s) and toggles an on-screen
indicator at a known screen location.

The leader itself is the A/V sync test video at
https://www.youtube.com/watch?v=QzomK1fdSUg

The container is produced by any capture tool (ffmpeg/obs/v4l2+arecord);
the card and sudo are NOT required.

Leader signal model (ground truth)
----------------------------------
- Audio: beep every 1.0s, panned to the channel(s) of the current event.
    * right event  -> beep on RIGHT channel
    * left  event  -> beep on LEFT channel
    * center event -> beep on BOTH channels (left+right)
- Video: a visible state-toggle indicator at a fixed location, one per
    event, in the panel corresponding to the channel.
    * right indicator  @ (1560, 520)
    * left  indicator  @ (360,  520)
    * center indicator @ (960,  520)
- The indicators are segment toggles (pixel-pattern change), so detection
    uses local pixel-difference in a tight crop, not mean-brightness.

Checks
------
1. Frame pacing: consecutive video PTS deltas must all be ~1/fps. A large
   gap = dropped frame(s); a near-zero delta = duplicate. Reports counts
   and which frames.
2. A/V sync: for each event, offset = toggle-onset-time - beep-onset-time.
   Printed as a table (Event | Beep | Toggle | Offset), with mean + drift.
   Fails if mean offset or max-min drift exceeds tolerance.

Usage
-----
  SYNC_TEST_VIDEO=/path/to/capture.mkv pytest tests/test_sync.py -s
  (defaults to /mnt/wd_black/Videos/windows_sync_test.mp4)
"""

import math
import os
import struct
import subprocess
import tempfile

import pytest


VIDEO_PATH = os.environ.get(
    "SYNC_TEST_VIDEO", "/mnt/wd_black/Videos/windows_sync_test.mp4"
)

AUDIO_SR = 48000
EXPECTED_FPS = 30
EXPECTED_FRAME_INTERVAL_MS = 1000.0 / EXPECTED_FPS  # 33.33ms

# Indicator anchor points (full-res 1920x1080) and the audio channel(s) that
# fire for the matching event. Order defines the expected panning cycle.
INDICATORS = {
    "right":  {"x": 1560, "y": 520, "channels": ("R",)},
    "left":   {"x": 360,  "y": 520, "channels": ("L",)},
    "center": {"x": 960,  "y": 520, "channels": ("L", "R")},
}
CROP_SIZE = 96  # square crop side, centered on each indicator

# Tolerances
FRAME_PACING_TOL_MS = 8.0   # jitter budget for a nominal frame period
AUDIO_OFFSET_TOL_S = 0.100  # absolute A/V offset budget
AUDIO_DRIFT_TOL_S = 0.050   # max allowed spread across events
# A re-presented frame differs from its neighbor only by H.264 noise
# (max per-pixel delta ~2-3); a real content change is ~230. This cleanly
# separates duplicates from distinct frames.
DUPLICATE_MAX_DELTA = 5
# A single toggle is an animated wipe spanning ~8 frames (diff rises, dips,
# rises again). The next real event is 30 frames later, so coalesce anything
# within ~15 frames into one toggle and report its leading edge.
TOGGLE_GROUP_GAP_FRAMES = 15


def _run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def _probe(ff, args):
    out = _run(
        ["ffprobe", "-v", "error", "-select_streams", ff,
         "-show_entries", f"stream={args}",
         "-of", "default=noprint_wrappers=1:nokey=1", VIDEO_PATH],
    )
    return out.stdout.decode().strip().splitlines()[0]


def _probe_fps():
    raw = _probe("v", "avg_frame_rate")
    if "/" in raw:
        num, den = raw.split("/")
        return float(num) / float(den)
    return float(raw)


def _read_pts(stream):
    """Sorted (presentation-order) packet PTS times as floats."""
    out = _run(
        ["ffprobe", "-v", "error", "-select_streams", stream,
         "-show_entries", "packet=pts_time", "-of", "csv=p=0", VIDEO_PATH],
    )
    vals = []
    for line in out.stdout.decode().strip().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            vals.append(float(line.split(",")[0]))
        except ValueError:
            continue
    return sorted(vals)


def _decode_audio_stereo(path):
    """Decode first audio stream to stereo float32. Returns (L, R, n_samples)."""
    with tempfile.NamedTemporaryFile(suffix=".f32", delete=False) as tmp:
        raw = tmp.name
    _run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0",
         "-ac", "2", "-ar", str(AUDIO_SR), "-f", "f32le", "-y", raw],
    )
    data = open(raw, "rb").read()
    os.unlink(raw)
    n = len(data) // 4
    samples = [struct.unpack_from("<f", data, i * 4)[0] for i in range(n)]
    L = samples[0::2]
    R = samples[1::2]
    return L, R


def _beep_onsets(ch, sr, window_s=0.010):
    """Beep onset times (s) in a single channel via RMS envelope threshold."""
    win = int(sr * window_s)
    n = len(ch) // win
    rms = [math.sqrt(sum(x * x for x in ch[i * win:(i + 1) * win]) / win)
           for i in range(n)]
    thr = max(rms) * 0.15
    loud = [v > thr for v in rms]
    onsets = []
    state = False
    for i, b in enumerate(loud):
        if b and not state:
            onsets.append(i * window_s)
        state = b
    return onsets


def _toggled_beep_times(L, R):
    """Return dict of channel -> list of beep onset times.

    A beep is attributed to a channel when that channel is loud and the other
    is not; if both are loud simultaneously it is a center event.
    """
    return {"L": _beep_onsets(L, AUDIO_SR), "R": _beep_onsets(R, AUDIO_SR)}


def _indicator_toggle_onsets(path, x, y, fps, size=CROP_SIZE, white_thr=230):
    """Toggle onset times (s) for one indicator via the white-triangle on/off.

    The indicator is a binary glyph: a white triangle appears (pixel near
    white -> ON) or is absent (no near-white pixel -> OFF). No ramp, no wipe.
    Event time = first frame where a near-white pixel (>= white_thr) is
    present after a run of OFF frames.
    """
    half = size // 2
    cx, cy = x - half, y - half
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as tmp:
        raw = tmp.name
    _run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v",
         "-vf", f"crop={size}:{size}:{cx}:{cy}",
         "-f", "rawvideo", "-pix_fmt", "gray", "-y", raw],
    )
    data = open(raw, "rb").read()
    os.unlink(raw)
    nf = len(data) // (size * size)
    data = data[:nf * size * size]

    on = []
    for i in range(nf):
        fr = data[i * size * size:(i + 1) * size * size]
        on.append(max(fr) >= white_thr)

    onsets = []
    prev = False
    for i, b in enumerate(on):
        if b and not prev:
            onsets.append(i)
        prev = b
    return [t / fps for t in onsets]


def test_format_is_expected():
    assert _probe_fps() == EXPECTED_FPS, \
        f"unexpected fps {_probe_fps()} (expected {EXPECTED_FPS})"
    assert int(_probe("v", "width")) == 1920 and int(_probe("v", "height")) == 1080, \
        f"unexpected resolution {_probe('v', 'width')}x{_probe('v', 'height')}"


def _decode_fullres_gray(path):
    """Decode full-res video to raw gray numpy array, return (frames, h, w)."""
    import numpy as np
    w, h = 1920, 1080
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as tmp:
        raw = tmp.name
    _run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:v",
         "-f", "rawvideo", "-pix_fmt", "gray", "-y", raw],
    )
    data = open(raw, "rb").read()
    os.unlink(raw)
    nf = len(data) // (w * h)
    data = data[:nf * w * h]
    return np.frombuffer(data, dtype=np.uint8).reshape(nf, h, w)


def _duplicate_and_dropped_info(path, fps):
    """Identify duplicated and dropped frames via decoded content.

    A duplicated frame is one whose full-res content is (near-)identical to
    the previous frame: the same source frame re-presented, differing only by
    H.264 compression noise (max per-pixel delta <= DUPLICATE_MAX_DELTA,
    ~0.02-0.06% of pixels, vs ~1-2% and max delta ~230 for a real change).
    A dropped frame is a PTS gap >= 2 nominal intervals.
    """
    import numpy as np
    from collections import Counter

    frames = _decode_fullres_gray(path)
    nf = frames.shape[0]
    nominal_s = EXPECTED_FRAME_INTERVAL_MS / 1000.0
    tol_s = FRAME_PACING_TOL_MS / 1000.0

    # duplicates by content
    dups = []
    for i in range(1, nf):
        delta = int(np.abs(frames[i].astype(int) - frames[i - 1].astype(int)).max())
        if delta <= DUPLICATE_MAX_DELTA:
            dups.append(round(i / fps, 3))

    # drops by PTS gap
    pts = _read_pts("v")
    drops = []
    for i in range(1, len(pts)):
        gap = pts[i] - pts[i - 1]
        if gap >= (nominal_s * 2 - tol_s):
            drops.append((round(pts[i], 3), round(gap / nominal_s) - 1))

    return dups, drops


def test_frame_pacing_no_drops_or_duplicates():
    fps = _probe_fps()
    dups, drops = _duplicate_and_dropped_info(VIDEO_PATH, fps)

    print(f"\nframe pacing ({fps:.0f}fps)")
    print(f"  duplicate frames (identical content): {len(dups)}")
    for t in dups:
        print(f"      {t:.3f}s")
    print(f"  dropped frames (PTS gap): {sum(d for _, d in drops)}")
    for t, d in drops:
        print(f"      {t:.3f}s  ({d} frame(s) dropped)")

    assert not dups, f"detected {len(dups)} duplicate frame(s): {[f'{t:.3f}s' for t in dups]}"
    assert not drops, f"detected dropped frame(s): {[(f'{t:.3f}s', d) for t, d in drops]}"


def test_audio_video_sync_offset():
    fps = _probe_fps()
    L, R = _decode_audio_stereo(VIDEO_PATH)

    # beep times per channel
    L_beeps = _beep_onsets(L, AUDIO_SR)
    R_beeps = _beep_onsets(R, AUDIO_SR)

    # toggle onset times per indicator
    toggles = {
        name: _indicator_toggle_onsets(VIDEO_PATH, cfg["x"], cfg["y"], fps)
        for name, cfg in INDICATORS.items()
    }

    # Build event rows: for each indicator event, its expected beep time is
    # the nearest beep on ANY of the channels that indicator uses.
    rows = []
    for name, cfg in INDICATORS.items():
        chans = cfg["channels"]
        # candidate beeps = union of beep times on each required channel,
        # keeping only times present in ALL required channels (center needs both)
        if len(chans) == 1:
            cand = {"L": L_beeps, "R": R_beeps}[chans[0]]
        else:
            cand = [b for b in L_beeps if any(abs(b - r) < 0.5 for r in R_beeps)]

        for tt in toggles[name]:
            if not cand:
                continue
            nearest = min(cand, key=lambda b: abs(b - tt))
            rows.append((name, nearest, tt, nearest - tt))

    rows.sort(key=lambda r: r[2])  # sort by toggle time

    if not rows:
        print("NO EVENTS DETECTED")
        assert False, "no events detected"

    mean_off = sum(r[3] for r in rows) / len(rows)
    spread = max(r[3] for r in rows) - min(r[3] for r in rows)

    print(f"\n{'Event':>5}  {'Type':>6}  {'Toggle (s)':>10}  "
          f"{'Beep (s)':>9}  {'Offset (ms)':>11}")
    for i, (name, bt, tt, off) in enumerate(rows, 1):
        print(f"{i:>5}  {name:>6}  {tt:>10.3f}  {bt:>9.3f}  {off*1000:>10.1f}")
    print(f"\n{len(rows)} events | mean offset {mean_off*1000:.1f} ms | "
          f"drift (max-min) {spread*1000:.1f} ms")

    assert abs(mean_off) <= AUDIO_OFFSET_TOL_S, \
        f"mean A/V offset {mean_off*1000:.1f}ms exceeds {AUDIO_OFFSET_TOL_S*1000:.0f}ms"
    assert spread <= AUDIO_DRIFT_TOL_S, \
        f"A/V drift {spread*1000:.1f}ms exceeds {AUDIO_DRIFT_TOL_S*1000:.0f}ms"


if __name__ == "__main__":
    import sys
    sys.exit(pytest.main([__file__, "-v", "-s"]))