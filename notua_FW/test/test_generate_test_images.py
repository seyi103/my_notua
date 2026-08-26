"""Tests for the Spectra 6 Y8 test-image generator."""

import tempfile
import unittest
from pathlib import Path

from scripts import generate_test_images as generator


class GenerateTestImagesTest(unittest.TestCase):
    def test_every_pattern_uses_only_the_spectra_palette(self) -> None:
        for pattern in range(3):
            values = {
                generator.pixel(pattern, x, y)
                for x in range(0, generator.WIDTH, 37)
                for y in range(0, generator.HEIGHT, 41)
            }
            self.assertLessEqual(values, generator.PALETTE_BYTES)

    def test_validate_image_accepts_exact_palette_frame(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "valid.bin"
            path.write_bytes(bytes([generator.SPECTRA_6_Y8_PALETTE[0]]) * generator.IMAGE_SIZE)
            generator.validate_image(path)

    def test_validate_image_rejects_wrong_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.bin"
            path.write_bytes(b"\x00")
            with self.assertRaisesRegex(ValueError, "expected 1920000"):
                generator.validate_image(path)

    def test_validate_image_rejects_value_outside_palette(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.bin"
            path.write_bytes(b"\x01" + b"\x00" * (generator.IMAGE_SIZE - 1))
            with self.assertRaisesRegex(ValueError, "0x01"):
                generator.validate_image(path)


if __name__ == "__main__":
    unittest.main()
