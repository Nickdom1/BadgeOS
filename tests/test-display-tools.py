#!/usr/bin/env python3
"""Offline capture failures and a sandboxed hold helper: never accesses a badge."""
import base64
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools/display"
spec = importlib.util.spec_from_file_location("capture", TOOLS / "validate-capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


class Capture(unittest.TestCase):
    def test_usage_error_is_not_diagnostic_evidence(self):
        command = [sys.executable, str(TOOLS / "validate-capture.py")]
        for arguments in ([], ["frames", "samples", "log", "--ffmpeg-exit", "", "--ffprobe-exit", "0"]):
            result = subprocess.run(command + arguments, capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, b"")

    def test_snapshot_transport_and_no_overwrite(self):
        blob = bytes(74000)
        record = (f"DISPLAY_CACHED_SNAPSHOT sha256={hashlib.sha256(blob).hexdigest()} bytes=74000\n"
                  "DISPLAY_BLOB_BEGIN\n" + base64.b64encode(blob).decode() + "\nDISPLAY_BLOB_END\n")
        with tempfile.TemporaryDirectory() as directory:
            log, output = Path(directory) / "badge.log", Path(directory) / "snapshot.bin"
            command = [sys.executable, str(TOOLS / "snapshot-from-log.py"), str(log), str(output)]
            for bad in (record + record, record.replace("DISPLAY_BLOB_END", ""),
                        record.replace("AAAA", "BAAA", 1), record.replace("AAAA", "AA!A", 1),
                        record.replace("bytes=74000", "bytes=1")):
                log.write_text(bad)
                self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
                self.assertFalse(output.exists())
            log.write_text(record)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            self.assertEqual(output.read_bytes(), blob)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            self.assertEqual(output.read_bytes(), blob)

    def test_timing_and_error_boundaries(self):
        pts = [i / 60 for i in range(900)]
        samples = [{"second": i, "verdict": "flat" if i < 2 else "pinstripe"} for i in range(15)]
        def check(values, evidence=samples, log="", rc=0):
            indexed = []
            previous = -1
            if len(values) >= 15:
                for sample in evidence:
                    index = min(range(previous + 1, len(values)), key=lambda n: abs(values[n] - sample["second"]))
                    indexed.append(dict(sample, frame_index=index, source_pts=values[index]))
                    previous = index
            return capture.validate({"frames": [{"best_effort_timestamp_time": str(p)} for p in values]}, indexed, log, rc)
        self.assertTrue(check(pts)["strict_valid"])
        for values in (pts[20:], [pts[0]] + pts[16:]):
            with self.subTest(startup=values[:2]):
                self.assertFalse(check(values)["strict_valid"])
                self.assertTrue(check(values)["diagnostic_valid"])
        bad = {"truncated": pts[:-20], "overlong": pts + [15 + i / 60 for i in range(20)],
               "late gap": pts[:200] + pts[205:], "repeated gaps": pts[:1] + pts[5:200] + pts[205:],
               "single dropped frame": pts[:200] + pts[201:], "late startup": pts[121:],
               "duplicate": pts[:100] + [pts[99]] + pts[100:], "nonfinite": [float("nan")] + pts,
               "negative": [-0.1] + pts, "50 Hz": [i / 50 for i in range(750)],
               "75 Hz": [i / 75 for i in range(1125)], "slow cadence": pts[::2], "missing": []}
        for name, values in bad.items():
            with self.subTest(case=name), self.assertRaises(ValueError):
                check(values)
        for evidence, log, rc in ((samples[:-1], "", 0), (samples, "corrupt buffer", 0),
                                  (samples, "", 1), (samples[:5] + [{"second": 5, "verdict": "hit"}] + samples[6:], "", 0)):
            with self.subTest(evidence_error=(len(evidence), log, rc)), self.assertRaises(ValueError):
                check(pts, evidence, log, rc)

    def test_sampler_checks_timestamps_against_real_clip(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            clip, frames, out = root / "clip.mkv", root / "frames.json", root / "samples"
            subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                            "color=black:size=64x64:rate=60", "-t", "15", "-c:v", "ffv1",
                            "-threads", "1", str(clip)], check=True, timeout=30)
            probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                                    "-show_frames", "-show_entries", "frame=best_effort_timestamp_time",
                                    "-of", "json", str(clip)], capture_output=True, text=True,
                                   check=True, timeout=30)
            frames.write_text(probe.stdout)
            command = [sys.executable, str(TOOLS / "sample-capture.py"), str(clip), str(frames), str(out)]
            subprocess.run(command, check=True, capture_output=True, timeout=60)
            samples = json.loads((out / "samples.json").read_text())
            self.assertEqual([s["second"] for s in samples], list(range(15)))
            self.assertEqual([s["frame_index"] for s in samples], list(range(0, 900, 60)))
            self.assertTrue(all(s["verdict"] == "flat" for s in samples))
            stale = json.loads(probe.stdout)
            for frame in stale["frames"]:
                frame["best_effort_timestamp_time"] = str(float(frame["best_effort_timestamp_time"]) * 2)
            frames.write_text(json.dumps(stale))
            command[-1] = str(root / "rejected")
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("timestamps do not match", result.stderr)
            self.assertFalse((root / "rejected").exists())

    def test_sample_labels_cannot_substitute_for_source_timing(self):
        frames = {"frames": [{"best_effort_timestamp_time": str(i / 60)} for i in range(900)]}
        samples = [{"second": i, "frame_index": i * 60, "source_pts": i, "verdict": "pinstripe"} for i in range(15)]
        for mutation in ({"source_pts": 99}, {"frame_index": 42}, {"frame_index": 120, "source_pts": 2}):
            broken = [dict(s) for s in samples]
            broken[5].update(mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                capture.validate(frames, broken, "")


class Hold(unittest.TestCase):
    def trial(self, fault=""):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bindir = root / "run/current-system/sw/bin"
            bindir.mkdir(parents=True)
            for name in ("cat", "sha256sum", "cut", "timeout", "mkdir", "wc", "base64"):
                (bindir / name).symlink_to(shutil.which(name))
            def write(path, data):
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data if isinstance(data, bytes) else data.encode())
            def command(name, text):
                target = bindir / name
                target.write_text("#!" + shutil.which("bash") + "\n" + text)
                target.chmod(0o700)
            old = "00000000-0000-0000-0000-000000000001"
            boot = "00000000-0000-0000-0000-000000000002"
            write("proc/sys/kernel/random/boot_id", boot)
            idle = "selected=1 terminal=0 started=0 owned=0 stage=idle error=0"
            owned = "selected=1 terminal=0 started=1 owned=1 stage=idle error=0"
            write("state", idle)
            command("uname", 'case "$1" in -m) echo riscv64;; -r) echo 7.2.2;; esac\n')
            command("sleep", "exit " + ("143" if fault == "interrupted-hold" else "0") + "\n")
            start_state = "selected=1 terminal=1 started=0 owned=1 stage=failed error=-5" if fault == "terminal" else owned
            command("mainline-pinstripe", f'''cd {shlex.quote(str(root))}
case "$1" in
status) [[ {shlex.quote(fault)} == unknown-state && -e actions ]] && exit 1; cat state;;
start) echo start >> actions; echo {shlex.quote(start_state)} > state; [[ {shlex.quote(fault)} != failed-start ]];;
stop) echo stop >> actions; echo {shlex.quote(idle)} > state; [[ {shlex.quote(fault)} != failed-stop ]];;
esac
''')
            for name in ("sophgo_dsi", "lontium_lt8912b"):
                write(f"sys/module/{name}/notes/.note.gnu.build-id", b"note")
            for param in ("pinstripe_trace", "pinstripe_snapshot", "pinstripe_dphy", "mac_fire_and_forget", "vip_bt_clock"):
                write(f"sys/module/sophgo_dsi/parameters/{param}", "N" if fault == param else "Y")
            write("sys/module/sophgo_dsi/parameters/mac_no_eot", "Y" if fault == "mac_no_eot" else "N")
            write("sys/firmware/fdt", b"fdt")
            write("sys/kernel/debug/dri/bridges", "[sophgo_dsi]\n[lontium_lt8912b]\n")
            write("sys/kernel/debug/sophgo-dsi/pinstripe-registers", b"\0" * (1 if fault == "short-snapshot" else 74000))
            if fault.startswith("marker:"):
                write("run/" + fault.split(":")[1], "used")
            if fault == "wrong-boot":
                write("proc/sys/kernel/random/boot_id", old)
            # Only the test copy changes root checks/absolute paths. Every file and
            # command then resolves inside this temporary fixture, never real sysfs.
            source = (TOOLS / "hold-once.sh").read_text().replace("$EUID == 0", "0 == 0")
            source = re.sub(r"(?<![\w/])/(run|sys|proc)/", lambda m: str(root) + m[0], source)
            script = root / "hold.sh"
            script.write_text(source)
            digest = lambda value: hashlib.sha256(value).hexdigest()
            args = [old, boot, digest(b"wrong" if fault == "wrong-note" else b"note"),
                    digest(b"wrong" if fault == "wrong-bridge" else b"note"),
                    digest(b"wrong" if fault == "wrong-fdt" else b"fdt")]
            result = subprocess.run(["bash", str(script), *args], capture_output=True, timeout=10)
            if fault == "repeat":
                result = subprocess.run(["bash", str(script), *args], capture_output=True, timeout=10)
            actions = (root / "actions").read_text().splitlines() if (root / "actions").exists() else []
            return result.returncode, actions, result.stderr.decode()

    def test_guards_do_not_activate(self):
        for fault in ("wrong-boot", "wrong-note", "wrong-bridge", "wrong-fdt", "pinstripe_trace", "pinstripe_snapshot", "pinstripe_dphy",
                      "mac_fire_and_forget", "vip_bt_clock", "mac_no_eot", "marker:badge-r9-start-attempted",
                      "marker:badge-r20-cycles", "marker:badge-display-start-attempted"):
            with self.subTest(fault=fault):
                code, actions, errors = self.trial(fault)
                self.assertNotEqual(code, 0)
                self.assertEqual(actions, [])

    def test_one_stop_even_after_owned_start_or_snapshot_failure(self):
        for fault in ("", "failed-start", "short-snapshot", "interrupted-hold", "failed-stop", "repeat"):
            with self.subTest(fault=fault):
                code, actions, errors = self.trial(fault)
                self.assertEqual(actions, ["start", "stop"], errors)
                self.assertEqual(code == 0, fault == "")

    def test_terminal_or_unknown_state_is_not_reentered(self):
        for fault in ("terminal", "unknown-state"):
            with self.subTest(fault=fault):
                code, actions, errors = self.trial(fault)
                self.assertNotEqual(code, 0)
                self.assertEqual(actions, ["start"])
                self.assertIn("DISPLAY_STOP_REFUSED", errors)


if __name__ == "__main__":
    unittest.main()
