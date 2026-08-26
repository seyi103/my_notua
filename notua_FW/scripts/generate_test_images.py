#!/usr/bin/env python3
"""Generate headerless 1600x1200 Y8 test patterns for LittleFS."""

import argparse
from pathlib import Path

WIDTH = 1600
HEIGHT = 1200


def pixel(pattern: int, x: int, y: int) -> int:
    if pattern == 0:
        return x * 255 // (WIDTH - 1)
    if pattern == 1:
        return 32 if ((x // 100) + (y // 100)) % 2 == 0 else 224
    return (x // 50 * 37 + y // 50 * 19) % 256


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("data/images"))
    parser.add_argument("--count", type=int, choices=(2, 3), default=3)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    for pattern in range(args.count):
        path = args.output / f"pattern_{pattern + 1:02d}.bin"
        with path.open("wb") as stream:
            for y in range(HEIGHT):
                stream.write(bytes(pixel(pattern, x, y) for x in range(WIDTH)))
        print(f"wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
