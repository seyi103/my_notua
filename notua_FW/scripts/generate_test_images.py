#!/usr/bin/env python3
"""Generate headerless 1600x1200 Y8 test patterns for LittleFS."""

import argparse
from pathlib import Path

WIDTH = 1600
HEIGHT = 1200
IMAGE_SIZE = WIDTH * HEIGHT
SPECTRA_6_Y8_PALETTE = (0x00, 0xF8, 0x20, 0x40, 0x10, 0x30)
PALETTE_BYTES = frozenset(SPECTRA_6_Y8_PALETTE)


def pixel(pattern: int, x: int, y: int) -> int:
    if pattern == 0:
        return SPECTRA_6_Y8_PALETTE[x * len(SPECTRA_6_Y8_PALETTE) // WIDTH]
    if pattern == 1:
        return SPECTRA_6_Y8_PALETTE[((x // 100) + (y // 100)) % 2]
    palette_index = (x // (30 + pattern * 10) * (31 + pattern * 3)
                     + y // (35 + pattern * 5) * (17 + pattern * 2)) % len(SPECTRA_6_Y8_PALETTE)
    return SPECTRA_6_Y8_PALETTE[palette_index]


def validate_image(path: Path) -> None:
    """Raise ValueError unless path is one complete palette-only Y8 frame."""
    size = path.stat().st_size
    if size != IMAGE_SIZE:
        raise ValueError(f"{path} is {size} bytes; expected {IMAGE_SIZE}")

    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(64 * 1024), b""):
            invalid = set(chunk) - PALETTE_BYTES
            if invalid:
                values = ", ".join(f"0x{value:02X}" for value in sorted(invalid))
                raise ValueError(
                    f"{path} contains values outside the Spectra 6 Y8 palette: {values}"
                )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("data/images"))
    parser.add_argument("--count", type=int, choices=range(1, 6), default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    for pattern in range(args.count):
        path = args.output / f"pattern_{pattern + 1:02d}.bin"
        with path.open("wb") as stream:
            for y in range(HEIGHT):
                stream.write(bytes(pixel(pattern, x, y) for x in range(WIDTH)))
        validate_image(path)
        print(f"wrote and verified {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
