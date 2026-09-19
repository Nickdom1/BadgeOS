"""Offline USB identity, metadata-node and ambiguity checks; no streaming."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("device", Path(__file__).resolve().parents[1] / "capture-device.py")
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


class Discovery(unittest.TestCase):
    def test_usb_filter_and_capture_capability(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sysfs = root / "class"
            sysfs.mkdir()
            for number, vendor, product in ((0, "345f", "2130"), (1, "345f", "2130"),
                                             (2, "04f2", "b6d9"), (3, "534d", "2109")):
                usb = root / f"usb{number}"
                node = usb / f"video{number}"
                node.mkdir(parents=True)
                (usb / "idVendor").write_text(vendor)
                (usb / "idProduct").write_text(product)
                (sysfs / node.name).symlink_to(node)
            with patch.object(device, "capture_capable", side_effect=lambda p: p.name != "video1") as check:
                self.assertEqual(device.discover(sysfs, root), [str(root / "video0"), str(root / "video3")])
                self.assertNotIn(root / "video2", [call.args[0] for call in check.call_args_list])
            with patch.object(device, "capture_capable", side_effect=PermissionError):
                self.assertEqual(device.discover(sysfs, root), [])

    def test_device_caps_override_union_caps(self):
        def ioctl(_handle, _request, caps, _mutate):
            struct.pack_into("=II", caps, 84, 0x84000001, 0x04800000)
        with tempfile.NamedTemporaryFile() as node, patch.object(device.fcntl, "ioctl", side_effect=ioctl):
            self.assertFalse(device.capture_capable(Path(node.name)))
        def capture_ioctl(_handle, _request, caps, _mutate):
            struct.pack_into("=II", caps, 84, 0x84000001, 0x04000001)
        with tempfile.NamedTemporaryFile() as node, patch.object(device.fcntl, "ioctl", side_effect=capture_ioctl):
            self.assertTrue(device.capture_capable(Path(node.name)))

    def test_missing_and_ambiguous_devices_refuse(self):
        for devices in ([], ["/dev/video0", "/dev/video2"]):
            with self.subTest(devices=devices), self.assertRaises(ValueError):
                device.choose(devices)
        self.assertEqual(device.choose(["/dev/video0"]), "/dev/video0")


if __name__ == "__main__":
    unittest.main()
