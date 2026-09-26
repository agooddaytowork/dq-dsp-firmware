#!/usr/bin/env python3
"""Exercise the release packager with realistic offsets and corrupted images."""

import tempfile
import unittest
from pathlib import Path

from tools.package_firmware import package


class PackageTest(unittest.TestCase):
    def test_offsets_and_checksums(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / "build"
            (build / "bootloader").mkdir(parents=True)
            (build / "partition_table").mkdir()
            boot = b"BOOT" * 16
            part = b"PART" * 16
            app = b"APP!" * 16
            (build / "bootloader/bootloader.bin").write_bytes(boot)
            (build / "partition_table/partition-table.bin").write_bytes(part)
            (build / "esp32s3_audio_dsp.bin").write_bytes(app)
            (build / "flash_args").write_text(
                "--flash_mode dio --flash_freq 80m --flash_size 16MB\n"
                "0x0 bootloader/bootloader.bin\n"
                "0x10000 esp32s3_audio_dsp.bin\n"
                "0x8000 partition_table/partition-table.bin\n"
            )
            merged = bytearray(b"\xff" * (0x10000 + len(app)))
            merged[:len(boot)] = boot
            merged[0x8000:0x8000 + len(part)] = part
            merged[0x10000:] = app
            image = root / "merged.bin"
            image.write_bytes(merged)
            out = root / "dist"
            package("1.2.0", build, image, out)
            self.assertEqual((out / "dq-dsp-firmware-1.2.0.bin").read_bytes(), merged)
            self.assertIn("0x8000 partition-table.bin", (out / "flash_args").read_text())
            self.assertEqual(len((out / "SHA256SUMS").read_text().splitlines()), 5)

            merged[0x8000] ^= 1
            image.write_bytes(merged)
            with self.assertRaisesRegex(ValueError, "differs"):
                package("1.2.0", build, image, root / "bad")
            with self.assertRaisesRegex(ValueError, "Invalid"):
                package("../bad", build, image, root / "bad")


if __name__ == "__main__":
    unittest.main()
