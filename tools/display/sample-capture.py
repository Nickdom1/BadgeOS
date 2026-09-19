#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Sample decoded frames near seconds 0..14, verifying the supplied clip timeline."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("clip", type=Path)
    parser.add_argument("frames", type=Path, help="FFprobe timestamps for the clip's first video stream")
    parser.add_argument("directory", type=Path, help="new output directory")
    args = parser.parse_args()
    clip, frames_file, directory = args.clip, args.frames, args.directory
    frames = json.loads(frames_file.read_text())["frames"]
    pts = [float(frame["best_effort_timestamp_time"]) for frame in frames]
    if len(pts) < 15 or not all(math.isfinite(p) and p >= 0 for p in pts):
        raise ValueError("missing or invalid source timestamps")
    # A stale frames.json must not lend another recording a passing timeline.
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                            "-show_frames", "-show_entries", "frame=best_effort_timestamp_time",
                            "-of", "json", str(clip)], capture_output=True, text=True, timeout=30)
    if probe.returncode or probe.stderr.strip():
        raise ValueError("clip timestamp probe failed: " + probe.stderr.strip())
    actual = [float(frame["best_effort_timestamp_time"])
              for frame in json.loads(probe.stdout)["frames"]]
    if pts != actual:
        raise ValueError("supplied timestamps do not match the clip")
    indices = []
    for second in range(15):
        first = indices[-1] + 1 if indices else 0
        indices.append(min(range(first, len(pts)), key=lambda index: abs(pts[index] - second)))
    directory.mkdir()  # Refuse to overwrite evidence.
    expression = "+".join(f"eq(n\\,{index})" for index in indices)
    with (directory / "extract.log").open("w") as log:
        subprocess.run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-xerror",
                        "-i", str(clip), "-map", "0:v:0", "-vf", "select=" + expression, "-fps_mode", "passthrough",
                        "-start_number", "0", "-n", str(directory / "frame-%02d.png")],
                       stderr=log, check=True, timeout=30)
    if (directory / "extract.log").read_text().strip():
        raise SystemExit("frame extraction emitted error diagnostics")
    if len(list(directory.glob("frame-*.png"))) != 15:
        raise SystemExit("frame extraction did not produce exactly 15 samples")
    results = []
    oracle = Path(__file__).with_name("oracle.py")
    for second, index in enumerate(indices):
        result = subprocess.run([sys.executable, str(oracle), "classify", str(directory / f"frame-{second:02d}.png")],
                                capture_output=True, text=True, timeout=10)
        if result.returncode not in (0, 10, 20) or result.stderr.strip():
            raise SystemExit("oracle failed: retain extraction directory for diagnosis")
        observation = json.loads(result.stdout)
        (directory / f"oracle-{second:02d}.json").write_text(result.stdout)
        observation.update(second=second, frame_index=index, source_pts=pts[index])
        results.append(observation)
    (directory / "samples.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
