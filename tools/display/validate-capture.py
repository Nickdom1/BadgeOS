#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only 15s/60Hz capture gate: exit 0 strict, 2 diagnostic-only, 1 invalid.

Samples JSON comes from sample-capture.py: actual oracle results for all 15
one-second samples, with decoded source frame indices/PTS, including startup.
No capture, frame generation, badge access or hardware acceptance is performed.
"""
import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path


_spec = importlib.util.spec_from_file_location("capture_timing", Path(__file__).with_name("capture_timing.py"))
_timing = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_timing)


def validate(frames, samples, stderr, ffmpeg_exit=0, ffprobe_exit=0):
    if stderr.strip() or ffmpeg_exit != 0 or ffprobe_exit != 0:
        raise ValueError("capture/decode failed or emitted error diagnostics")
    result = _timing.validate_timing(frames)
    pts = [float(f["best_effort_timestamp_time"]) for f in frames["frames"]]
    tolerance = result["tolerance"]
    if (not isinstance(samples, list) or len(samples) != 15 or
            any(not isinstance(s, dict) or type(s.get("second")) is not int for s in samples) or
            [s.get("second") for s in samples] != list(range(15)) or
            any(s.get("verdict") not in ("pinstripe", "flat", "hit") for s in samples)):
        raise ValueError("expected all 15 ordered one-second oracle results")
    previous = -1
    for sample in samples:
        index = sample.get("frame_index")
        timestamp = sample.get("source_pts")
        if (type(index) is not int or not previous < index < len(pts) or
                not isinstance(timestamp, (int, float)) or not math.isfinite(timestamp) or
                abs(timestamp - pts[index]) > 0.000001):
            raise ValueError("sample does not identify an ordered decoded source frame")
        if sample["second"] >= 2 and abs(timestamp - sample["second"]) > tolerance:
            raise ValueError("settled sample does not cover its declared second")
        previous = index
    if any(s["verdict"] != "pinstripe" for s in samples[2:]):
        raise ValueError("settled sample is not pinstripe")
    return dict(result, settled_pinstripe_samples=13)


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # Exit 2 is reserved for successfully parsed diagnostic evidence.
        self.print_usage(sys.stderr)
        self.exit(1, f"{self.prog}: error: {message}\n")


def main():
    parser = Parser(description=__doc__)
    parser.add_argument("frames", type=Path)
    parser.add_argument("samples", type=Path)
    parser.add_argument("stderr", type=Path, help="combined ffmpeg/ffprobe -loglevel error output")
    parser.add_argument("--ffmpeg-exit", type=int, required=True)
    parser.add_argument("--ffprobe-exit", type=int, required=True)
    args = parser.parse_args()
    try:
        result = validate(json.loads(args.frames.read_text()), json.loads(args.samples.read_text()),
                          args.stderr.read_text(), args.ffmpeg_exit, args.ffprobe_exit)
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(json.dumps({"strict_valid": False, "diagnostic_valid": False, "error": str(error)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0 if result["strict_valid"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
