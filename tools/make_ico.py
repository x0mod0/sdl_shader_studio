#!/usr/bin/env python3
"""Pack PNGs into a Windows .ico.

Regenerates assets/icons/app.ico, the icon compiled into the Windows
executable by assets/icons/app.rc. The output is checked in so a Windows
build needs no image tooling; run this only when the artwork changes:

    python3 tools/make_ico.py

Entries are stored as PNG, which every Windows version since Vista reads
directly. Sizes come from assets/icons/square (full bleed: Windows draws its
own rounding in the taskbar and title bar).
"""
import struct
import sys
from pathlib import Path

SIZES = (16, 32, 48, 64, 128, 256)


def png_size(data: bytes) -> tuple[int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    return struct.unpack(">II", data[16:24])


def build_ico(paths: list[Path]) -> bytes:
    images = []
    for path in paths:
        data = path.read_bytes()
        width, height = png_size(data)
        if width != height:
            raise ValueError(f"{path}: icon entries must be square, got {width}x{height}")
        if not 1 <= width <= 256:
            raise ValueError(f"{path}: {width}px is outside the 1..256 an .ico can hold")
        images.append((width, data))

    # 6-byte ICONDIR, then one 16-byte ICONDIRENTRY each, then the payloads.
    offset = 6 + 16 * len(images)
    directory, payload = b"", b""
    for width, data in images:
        # A dimension of 256 is stored as 0; 0 planes/bpp means "ask the PNG".
        directory += struct.pack(
            "<BBBBHHII", width % 256, width % 256, 0, 0, 1, 32, len(data), offset)
        payload += data
        offset += len(data)
    return struct.pack("<HHH", 0, 1, len(images)) + directory + payload


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    source = root / "assets" / "icons" / "square"
    output = root / "assets" / "icons" / "app.ico"

    paths = [source / f"icon-{size}.png" for size in SIZES]
    missing = [p for p in paths if not p.is_file()]
    if missing:
        # 48 is optional: the square set does not always ship one.
        paths = [p for p in paths if p.is_file()]
        for p in missing:
            print(f"note: skipping absent {p.relative_to(root)}", file=sys.stderr)
    if not paths:
        print(f"error: no source PNGs in {source}", file=sys.stderr)
        return 1

    output.write_bytes(build_ico(paths))
    print(f"wrote {output.relative_to(root)} "
          f"({', '.join(str(png_size(p.read_bytes())[0]) for p in paths)} px)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
