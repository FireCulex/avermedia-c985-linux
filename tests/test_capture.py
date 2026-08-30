#!/usr/bin/env python3
"""
Pytest test for AVerMedia C985 video capture verification.

Captures 30 frames at 1080p (YUV420) and verifies:
- Frames arrive at ~30fps (33ms intervals) based on buffer timestamps
- Resolution is 1920x1080
- Pixel format is YUV420
- Sequence numbers increment monotonically (no duplicates/drops)
- Frames are visually different (image comparison)

Uses v4l2-ctl with --verbose --stream-show-delta-now to get buffer timestamps and sequence numbers.
Device is pre-configured by c985-bind.sh to 1920x1080 YU12 @ 30fps.
"""

import os
import subprocess
import re
import pytest
import tempfile
import hashlib


DEVICE_PATH = os.environ.get("C985_DEVICE", "/dev/video0")
EXPECTED_WIDTH = 1920
EXPECTED_HEIGHT = 1080
EXPECTED_FMT = "YU12"
FRAME_COUNT = 30  # Always capture 30 frames
FPS_TOLERANCE = 0.15
CAPTURE_TIMEOUT_S = 5.0  # hard timeout on the streaming capture, not the pytest one
EXPECTED_FRAME_SIZE = EXPECTED_WIDTH * EXPECTED_HEIGHT * 3 // 2
EXPECTED_INTERVAL_MS = 1000 / 30  # 33.33ms for 30fps


def run_v4l2_ctl(args, timeout=None):
    """Run v4l2-ctl and return combined stdout+stderr."""
    cmd = ["v4l2-ctl", "--device", DEVICE_PATH] + args
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"v4l2-ctl failed: {result.stderr}")
    return result.stdout + result.stderr


def parse_stream_output(output):
    """Parse v4l2-ctl streaming output for sequence numbers and timestamps."""
    pattern = r'cap dqbuf:\s+\d+\s+seq:\s+(\d+)\s+bytesused:\s+\d+\s+ts:\s+[\d.]+(?:\s+delta:\s+([\d.]+)\s+ms)?'
    matches = re.findall(pattern, output)
    
    sequences = []
    deltas = []
    for match in matches:
        seq = int(match[0])
        sequences.append(seq)
        if match[1]:
            deltas.append(float(match[1]))
    
    return sequences, deltas


def compute_frame_diffs(filepath, frame_count, frame_size):
    """Compute pixel difference percentage between consecutive frames."""
    diffs = []
    y_size = EXPECTED_WIDTH * EXPECTED_HEIGHT
    with open(filepath, 'rb') as f:
        prev_y = None
        for i in range(frame_count):
            frame_data = f.read(frame_size)
            if len(frame_data) != frame_size:
                break
            y_plane = frame_data[:y_size]
            if prev_y is not None:
                diff = sum(1 for a, b in zip(y_plane, prev_y) if a != b)
                pct = diff / len(y_plane) * 100
                diffs.append(pct)
            prev_y = y_plane
    return diffs


def compute_effective_fps(filepath, frame_count, frame_size, threshold_pct=0.5):
    """Compute effective FPS based on pixel difference threshold."""
    diffs = compute_frame_diffs(filepath, frame_count, frame_size)
    unique_count = 1  # First frame is always unique
    for pct in diffs:
        if pct > threshold_pct:
            unique_count += 1
    return unique_count / frame_count * 30


def test_device_exists():
    """Verify device exists."""
    assert os.path.exists(DEVICE_PATH), f"Device {DEVICE_PATH} not found"


def test_format_verification():
    """Verify device format is 1920x1080 YU12."""
    output = run_v4l2_ctl(["--all"])
    
    assert f"Width/Height      : {EXPECTED_WIDTH}/{EXPECTED_HEIGHT}" in output
    assert f"Pixel Format      : '{EXPECTED_FMT}'" in output
    assert "Field             : None" in output
    assert f"Size Image        : {EXPECTED_FRAME_SIZE}" in output


def test_capture_30_frames_verify_timing_and_sequences():
    """Capture 30 frames and verify 30fps timing from buffer timestamps and monotonic sequences.
    Also performs image comparison to detect visual duplicates."""
    with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
        tmp_path = tmp.name
    
    try:
        try:
            output = run_v4l2_ctl([
                "--stream-mmap",
                f"--stream-count={FRAME_COUNT}",
                f"--stream-to={tmp_path}",
                "--stream-show-delta-now",
                "--verbose"
            ], timeout=CAPTURE_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            pytest.fail(
                f"v4l2 capture did not produce {FRAME_COUNT} frames within "
                f"{CAPTURE_TIMEOUT_S}s (card stalled / no frames)")
        
        sequences, deltas = parse_stream_output(output)
        
        print(f"\nCaptured {len(sequences)} frames")
        print(f"Sequences: {sequences[:10]}{'...' if len(sequences) > 10 else ''}")
        if deltas:
            print(f"Frame intervals (ms): {[f'{d:.1f}' for d in deltas[:10]]}{'...' if len(deltas) > 10 else ''}")
            avg_delta = sum(deltas) / len(deltas)
            actual_fps = 1000 / avg_delta
            print(f"Average interval: {avg_delta:.2f}ms")
            print(f"Average FPS: {actual_fps:.2f}")
        
        # Verify we got 30 frames
        assert len(sequences) == FRAME_COUNT, \
            f"Expected {FRAME_COUNT} frames, got {len(sequences)}"
        
        # Verify sequence numbers are strictly monotonic (no duplicates, no drops)
        for i in range(1, len(sequences)):
            assert sequences[i] == sequences[i-1] + 1, \
                f"Sequence gap/duplicate: {sequences[i-1]} -> {sequences[i]}"
        
        # Verify frame intervals are within tolerance of 33.33ms (30fps)
        assert len(deltas) == FRAME_COUNT - 1, \
            f"Expected {FRAME_COUNT - 1} deltas, got {len(deltas)}"
        
        for i, delta_ms in enumerate(deltas):
            lower = EXPECTED_INTERVAL_MS * (1 - FPS_TOLERANCE)
            upper = EXPECTED_INTERVAL_MS * (1 + FPS_TOLERANCE)
            assert lower <= delta_ms <= upper, \
                f"Frame {i} interval {delta_ms:.2f}ms outside [{lower:.1f}, {upper:.1f}]ms"
        
# Image comparison: verify frames are visually different
        diffs = compute_frame_diffs(tmp_path, FRAME_COUNT, EXPECTED_FRAME_SIZE)
        
        # Effective FPS
        effective_fps = compute_effective_fps(tmp_path, FRAME_COUNT, EXPECTED_FRAME_SIZE)
        unique_count = int(effective_fps / 30 * FRAME_COUNT)
        print(f"\nEffective FPS: {effective_fps:.1f}fps ({unique_count}/{FRAME_COUNT} unique frames)")
        
        # Fail if effective FPS is too low (static/black frame)
        assert effective_fps >= 15, f"Effective FPS {effective_fps:.1f} below threshold (15fps) - likely static/black frame"
        
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


def test_streaming_parameters():
    """Report streaming parameters (informational)."""
    output = run_v4l2_ctl(["--get-parm"])
    assert "timeperframe" in output.lower()


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])