#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Find a supported HDMI capture node without starting a stream or choosing a webcam."""
import argparse
import fcntl
import json
from pathlib import Path
import struct
import sys

USB_IDS = {("345f", "2130"), ("534d", "2109")}


def capture_capable(device):
    # Linux videodev2.h: VIDIOC_QUERYCAP and struct v4l2_capability.
    caps = bytearray(104)
    with device.open("rb", buffering=0) as handle:
        fcntl.ioctl(handle, 0x80685600, caps, True)
    capabilities, device_caps = struct.unpack_from("=II", caps, 84)
    if capabilities & 0x80000000:  # V4L2_CAP_DEVICE_CAPS
        capabilities = device_caps
    return bool(capabilities & 0x00000001 and capabilities & 0x04000000)


def discover(sysfs=Path("/sys/class/video4linux"), devroot=Path("/dev")):
    devices = []
    for node in sorted(sysfs.glob("video*")):
        try:
            for parent in node.resolve().parents:
                if (parent / "idVendor").exists():
                    identity = tuple((parent / name).read_text().strip().lower()
                                     for name in ("idVendor", "idProduct"))
                    break
            else:
                continue
            if identity in USB_IDS and capture_capable(devroot / node.name):
                devices.append(str(devroot / node.name))
        except OSError:
            continue  # Disappeared or inaccessible; never substitute a webcam.
    return devices


def choose(devices):
    if len(devices) != 1:
        raise ValueError(f"expected one accessible HDMI capture node, found {len(devices)}; use --list")
    return devices[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="list all matching accessible capture nodes as JSON")
    args = parser.parse_args()
    devices = discover()
    if args.list:
        print(json.dumps(devices))
    else:
        print(choose(devices))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(str(error))
