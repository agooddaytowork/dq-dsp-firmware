#!/usr/bin/env python3
"""Verify and stage an ESP-IDF build as individually flashable release assets."""

import argparse
import hashlib
import re
import shlex
from pathlib import Path

EXPECTED = {
    0x0: ("bootloader/bootloader.bin", "bootloader.bin"),
    0x8000: ("partition_table/partition-table.bin", "partition-table.bin"),
    0x10000: ("esp32s3_audio_dsp.bin", "esp32s3_audio_dsp.bin"),
}


def package(version: str, build: Path, merged: Path, out: Path) -> None:
    if not re.fullmatch(r"(?:[0-9]+\.[0-9]+\.[0-9]+|dev-[0-9a-f]{7})", version):
        raise ValueError(f"Invalid firmware version: {version}")
    if out.exists() and any(out.iterdir()):
        raise ValueError(f"Output directory must be empty: {out}")

    lines = (build / "flash_args").read_text().splitlines()
    if len(lines) != 4:
        raise ValueError("Expected flash settings and three component images")
    settings = shlex.split(lines[0])
    if settings != ["--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"]:
        raise ValueError(f"Unexpected flash settings: {settings}")

    image = merged.read_bytes()
    mappings = [shlex.split(line) for line in lines[1:]]
    expected_mappings = {
        (hex(offset), relative) for offset, (relative, _) in EXPECTED.items()
    }
    if {tuple(fields) for fields in mappings} != expected_mappings or len(mappings) != len(EXPECTED):
        raise ValueError(f"Unexpected flash image mappings: {mappings}")

    staged = []
    for offset, (relative, name) in EXPECTED.items():
        data = (build / relative).read_bytes()
        if image[offset:offset + len(data)] != data:
            raise ValueError(f"Merged image differs from {relative} at {hex(offset)}")
        staged.append((name, data))

    out.mkdir(parents=True, exist_ok=True)
    merged_name = f"dq-dsp-firmware-{version}.bin"
    (out / merged_name).write_bytes(image)
    for name, data in staged:
        (out / name).write_bytes(data)
    (out / "flash_args").write_text(
        lines[0] + "\n" + "\n".join(
            f"{hex(offset)} {name}" for offset, (_, name) in EXPECTED.items()
        ) + "\n"
    )
    checksums = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in sorted(out.iterdir()) if path.is_file()
    )
    (out / "SHA256SUMS").write_text(checksums)
    print(f"Packaged {merged_name} ({len(image)} bytes) and verified three flash offsets")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--merged", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    package(args.version, args.build_dir, args.merged, args.out_dir)
