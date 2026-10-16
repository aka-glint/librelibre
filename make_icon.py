#!/usr/bin/env python3
"""Generate the 10x10 1-bit monochrome PNG icon for LibreLibre.

Design: a blood-drop / sensor glyph. Points up, rounds at the bottom.
Pure stdlib (zlib + struct) so it needs no PIL.
"""
import struct
import zlib
from pathlib import Path

DESIGN = [
    "..........",
    "....XX....",
    "....XX....",
    "...XXXX...",
    "...XXXX...",
    "..XXXXXX..",
    ".XXXXXXXX.",
    ".XXXXXXXX.",
    ".XXXXXXXX.",
    "..XXXXXX..",
]


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def build() -> bytes:
    height = len(DESIGN)
    width = max(len(r) for r in DESIGN)
    assert (width, height) == (10, 10), (width, height)

    raw = bytearray()
    for row in DESIGN:
        assert len(row) == width
        raw.append(0)  # filter type: None
        bitbuf = 0
        nbits = 0
        for cell in row:
            on = 1 if cell == "X" else 0
            # 1-bit greyscale: 0 = black (drawn), 1 = white
            value = 0 if on else 1
            bitbuf = (bitbuf << 1) | value
            nbits += 1
            if nbits == 8:
                raw.append(bitbuf)
                bitbuf = 0
                nbits = 0
        if nbits:
            raw.append(bitbuf << (8 - nbits))

    ihdr = struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


if __name__ == "__main__":
    out = Path(__file__).with_name("librelibre_icon.png")
    out.write_bytes(build())
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    for row in DESIGN:
        print("  " + row.replace(".", " ").replace("X", "#"))
