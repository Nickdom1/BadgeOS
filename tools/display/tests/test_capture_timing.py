"""Shared timing contract; synthetic clips, no capture hardware or raw evidence."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('timing', Path(__file__).resolve().parents[1] / 'capture_timing.py')
timing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timing)


def frames(pts):
    return {'frames': [{'best_effort_timestamp_time': p} for p in pts]}


class Timing(unittest.TestCase):
    def test_strict_60_and_5994(self):
        for rate in (60, 60000 / 1001):
            pts = [round(i / rate, 3) for i in range(round(15 * rate))]
            self.assertTrue(timing.validate_timing(frames(pts))['strict_valid'])

    def test_reference_shaped_startup_is_diagnostic_only(self):
        # Reference first gaps span 51..317ms; the short interval is 4..5ms.
        for first_gap in (.051, .243, .251, .256, .278, .317):
            pts = [0.0] + [round(first_gap + i / 60, 3)
                           for i in range(round((15 - first_gap) * 60))]
            pts[58] = pts[57] + .004
            result = timing.validate_timing(frames(pts))
            self.assertFalse(result['strict_valid'])
            self.assertTrue(result['diagnostic_valid'])
            self.assertGreaterEqual(len(result['startup_irregular_intervals']), 2)

    def test_late_or_boundary_crossing_faults_refuse(self):
        pts = [i / 60 for i in range(900)]
        del pts[120:123]  # Monotonic gap from 1.983s to 2.050s crosses settling.
        with self.assertRaisesRegex(ValueError, 'after settling'):
            timing.validate_timing(frames(pts))
        pts = [i / 60 for i in range(900)]
        pts[201] = pts[200] + .004
        with self.assertRaisesRegex(ValueError, 'after settling'):
            timing.validate_timing(frames(pts))
        pts = [i / 60 for i in range(900)]
        del pts[200]
        with self.assertRaises(ValueError):
            timing.validate_timing(frames(pts))

    def test_invalid_timestamps_and_rates_refuse(self):
        nominal = [i / 60 for i in range(900)]
        cases = [[], [-.001] + nominal[1:], [float('nan')] + nominal[1:],
                 nominal[:50] + [nominal[49]] + nominal[50:], nominal[:-30],
                 nominal + [15 + i / 60 for i in range(30)], nominal[121:]]
        cases += [[i / rate for i in range(15 * rate)] for rate in (25, 30, 50, 75)]
        for pts in cases:
            with self.subTest(start=pts[:2], length=len(pts)), self.assertRaises(ValueError):
                timing.validate_timing(frames(pts))


if __name__ == '__main__':
    unittest.main()
