#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract one hash-checked cached snapshot from hold-once.sh output."""
import argparse
import base64
import hashlib
from pathlib import Path
import re


def extract(text):
    if any(text.splitlines().count(marker) != 1
           for marker in ("DISPLAY_BLOB_BEGIN", "DISPLAY_BLOB_END")):
        raise ValueError("expected exactly one snapshot block")
    headers = re.findall(r"^DISPLAY_CACHED_SNAPSHOT sha256=([0-9a-f]{64}) bytes=74000$",
                         text, re.M)
    if len(headers) != 1:
        raise ValueError("expected one snapshot hash/size record")
    match = re.search(r"^DISPLAY_BLOB_BEGIN\n([A-Za-z0-9+/=\n]+)\nDISPLAY_BLOB_END$", text, re.M)
    if not match:
        raise ValueError("malformed snapshot block")
    blob = base64.b64decode(match[1].replace("\n", ""), validate=True)
    if len(blob) != 74000 or hashlib.sha256(blob).hexdigest() != headers[0]:
        raise ValueError("snapshot size/hash mismatch")
    return blob


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        blob = extract(args.log.read_text())
        with args.output.open("xb") as output:
            output.write(blob)
    except (ValueError, OSError) as error:
        parser.exit(1, f"snapshot refused: {error}\n")
    print(f"snapshot bytes={len(blob)} sha256={hashlib.sha256(blob).hexdigest()}")


if __name__ == "__main__":
    main()
