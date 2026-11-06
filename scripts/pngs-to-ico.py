#!/usr/bin/env python3
"""Pack PNG images into a Windows ICO file using only the standard library."""

import struct
import sys
from pathlib import Path


def dimensions(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("input is not a PNG image")
    return struct.unpack(">II", data[16:24])


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: pngs-to-ico.py OUTPUT.ico IMAGE.png...")

    images = []
    for filename in sorted(sys.argv[2:]):
        data = Path(filename).read_bytes()
        width, height = dimensions(data)
        if width != height or width > 256:
            raise ValueError(f"unsupported icon dimensions: {filename}")
        images.append((width, height, data))

    offset = 6 + 16 * len(images)
    entries = []
    payload = []
    for width, height, data in images:
        entries.append(struct.pack(
            "<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32,
            len(data), offset))
        payload.append(data)
        offset += len(data)

    output = Path(sys.argv[1])
    output.write_bytes(struct.pack("<HHH", 0, 1, len(images)) +
                       b"".join(entries) + b"".join(payload))


if __name__ == "__main__":
    main()
