# SPDX-License-Identifier: GPL-2.0-only
"""15-second, nominal-60-Hz capture timing; startup evidence is never a strict pass."""
import math
import statistics


def validate_timing(data, requested=15.0):
    pts = [float(f['best_effort_timestamp_time']) for f in data.get('frames', [])]
    if len(pts) < 2 or not all(math.isfinite(p) and p >= 0 for p in pts):
        raise ValueError('capture timestamps missing/nonfinite/negative')
    intervals = [b - a for a, b in zip(pts, pts[1:])]
    if min(intervals) <= 0:
        raise ValueError('capture timestamps not increasing')
    interval = statistics.median(intervals)
    # Matroska timestamps are commonly rounded to milliseconds at 60/59.94 Hz.
    if abs(interval - 1 / 60) > 0.001:
        raise ValueError('capture gaps/cadence: expected nominal 60 Hz')
    tolerance = min(0.1, 2 * interval + 0.002)
    end = pts[-1] + interval
    if abs(end - requested) > tolerance:
        raise ValueError('capture end coverage')
    irregular = [i for i, dt in enumerate(intervals)
                 if dt > 1.5 * interval + 0.001 or dt < 0.5 * interval - 0.001]
    startup_gap = intervals[0] if 0 in irregular else None
    # Reference clips have both a leading gap and a short interval during
    # settling. Keep them diagnostic; faults crossing or following 2s refuse.
    if startup_gap is not None and pts[1] > 2:
        raise ValueError('capture startup exceeds bounded warmup')
    if any(pts[i + 1] > 2 for i in irregular):
        raise ValueError('capture gaps/cadence after settling')
    startup_end = max([pts[0]] + [pts[i + 1] for i in irregular])
    if pts[0] > 2 or end - startup_end < requested - 2:
        raise ValueError('capture startup exceeds bounded warmup')
    strict = pts[0] <= tolerance and not irregular
    return {'strict_valid': strict, 'diagnostic_valid': True,
            'caveat': None if strict else 'startup timestamp anomaly within 2s; not strict acceptance',
            'frames': len(pts), 'first_pts': pts[0], 'end_pts': end,
            'median_interval': interval, 'max_interval': max(intervals),
            'startup_gap': startup_gap, 'tolerance': tolerance,
            'startup_irregular_intervals': [
                {'frame_index': i, 'from_pts': pts[i], 'to_pts': pts[i + 1],
                 'interval': intervals[i]} for i in irregular]}
