#!/usr/bin/env python3
"""Decode the text .b64 release files back into binaries and check SHA256SUMS.

The GitHub connector used for this release can only push text, so the
prebuilt images and preview PNGs are stored as base64. This script restores
the files SHA256SUMS refers to.
"""
import base64
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = [
    ROOT / "releases/v1.0.1/companion-v1.0.1.bin",
    ROOT / "releases/v1.0.1/assets-v1.0.1.bin",
    ROOT / "releases/v1.0.1/bootloader.bin",
    ROOT / "releases/v1.0.1/partition-table.bin",
    ROOT / "docs/v1.0.1-expressions-preview.png",
    ROOT / "docs/v1.0.1-frames-preview.png",
]

def main():
    for dest in TARGETS:
        src = Path(str(dest) + ".b64")
        if not src.exists():
            raise SystemExit(f"missing {src}")
        dest.write_bytes(base64.b64decode(src.read_text()))
        print(f"wrote {dest} {dest.stat().st_size} bytes")
    assets = ROOT / "releases/v1.0.1/assets-v1.0.1.bin"
    copy = ROOT / "firmware/idf/assets.bin"
    copy.write_bytes(assets.read_bytes())
    print(f"wrote {copy} {copy.stat().st_size} bytes")
    sums = ROOT / "releases/v1.0.1/SHA256SUMS"
    ok = True
    for line in sums.read_text().splitlines():
        expected, name = line.split()
        path = sums.parent / name
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        status = "OK" if got == expected else "FAIL"
        if status != "OK":
            ok = False
        print(f"{status} {name}")
    if not ok:
        raise SystemExit("SHA256 mismatch")
    print("all checksums ok")

if __name__ == "__main__":
    main()
