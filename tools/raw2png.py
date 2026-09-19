#!/usr/bin/env python3
"""
raw2png.py — convert a 256x240 NES index buffer (written by the host
runner) into a PNG with the NES master palette.

    python3 tools/raw2png.py build/frame.raw build/frame.png [scale]
"""
import struct
import sys
import zlib

# the standard 2C02 palette (64 colours), widely used by emulators
NES_PALETTE = [
    (84, 84, 84), (0, 30, 116), (8, 16, 144), (48, 0, 136),
    (68, 0, 100), (92, 0, 48), (84, 4, 0), (60, 24, 0),
    (32, 42, 0), (8, 58, 0), (0, 64, 0), (0, 60, 0),
    (0, 50, 60), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (152, 150, 152), (8, 76, 196), (48, 50, 236), (92, 30, 228),
    (136, 20, 176), (160, 20, 100), (152, 34, 32), (120, 60, 0),
    (84, 90, 0), (40, 114, 0), (8, 124, 0), (0, 118, 40),
    (0, 102, 120), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (76, 154, 236), (120, 124, 236), (176, 98, 236),
    (228, 84, 236), (236, 88, 180), (236, 106, 100), (212, 136, 32),
    (160, 170, 0), (116, 196, 0), (76, 208, 32), (56, 204, 108),
    (56, 180, 204), (60, 60, 60), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (168, 204, 236), (188, 188, 236), (212, 178, 236),
    (236, 174, 236), (236, 174, 212), (236, 180, 176), (228, 196, 144),
    (204, 210, 120), (180, 222, 120), (168, 226, 144), (152, 226, 180),
    (160, 214, 228), (160, 162, 160), (0, 0, 0), (0, 0, 0),
]


def chunk(tag, data):
    c = struct.pack(">I", len(data)) + tag + data
    return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "build/frame.raw"
    dst = sys.argv[2] if len(sys.argv) > 2 else "build/frame.png"
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    data = open(src, "rb").read()
    w, h = 256, 240
    assert len(data) >= w * h, f"{src}: {len(data)} bytes, expected {w*h}"

    pal = []
    for r, g, b in NES_PALETTE:
        pal += [r, g, b]
    rows = b""
    for y in range(h):
        row = bytes(data[y * w:(y + 1) * w])
        if scale > 1:
            row = b"".join(bytes([p]) * scale for p in row)
        for _ in range(scale):
            rows += b"\x00" + row

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale,
                                        8, 3, 0, 0, 0))
           + chunk(b"PLTE", bytes(pal))
           + chunk(b"IDAT", zlib.compress(rows, 9))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    print(f"wrote {dst} ({w*scale}x{h*scale})")


if __name__ == "__main__":
    main()
