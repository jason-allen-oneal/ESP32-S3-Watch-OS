#!/usr/bin/env python3
"""Package a built Nightglass app image and its canonical OTA manifest payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import struct


BOARD_ID = "nightglass-waveshare-esp32s3r8-2.06-v1.0"
PARTITION_ID = "nightglass-32m-r1"
SLOT_SIZE = 6 * 1024 * 1024
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def fixed_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def canonical_payload(manifest: dict[str, object]) -> str:
    return (
        "NIGHTGLASS-OTA-MANIFEST\n"
        f"format={manifest['format_version']}\n"
        f"board={manifest['board_id']}\n"
        f"partition={manifest['partition_id']}\n"
        f"partition_revision={manifest['partition_revision']}\n"
        f"version={manifest['app_version']}\n"
        f"secure_version={manifest['secure_version']}\n"
        f"size={manifest['image_size']}\n"
        f"sha256={manifest['image_sha256']}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--signature", type=pathlib.Path)
    parser.add_argument("--signature-algorithm")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    image = args.image.read_bytes()
    if len(image) == 0 or len(image) > SLOT_SIZE:
        raise SystemExit("image is empty or exceeds the 6 MiB OTA slot")
    if image[0] != 0xE9 or len(image) < APP_DESC_OFFSET + 80:
        raise SystemExit("input is not an ESP application image")
    magic, secure_version = struct.unpack_from("<II", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise SystemExit("ESP application descriptor not found at expected offset")
    app_version = fixed_string(image[APP_DESC_OFFSET + 16 : APP_DESC_OFFSET + 48])
    project_name = fixed_string(image[APP_DESC_OFFSET + 48 : APP_DESC_OFFSET + 80])
    if project_name != "nightglass" or not app_version:
        raise SystemExit("image is not a versioned Nightglass application")
    if len(app_version.encode()) >= 32:
        raise SystemExit("embedded application version is not terminated")
    if args.signature and not args.signature_algorithm:
        raise SystemExit("--signature-algorithm is required with --signature")
    if args.signature_algorithm and not args.signature:
        raise SystemExit("--signature is required with --signature-algorithm")

    outputs = [args.output / "firmware.bin", args.output / "manifest.json", args.output / "manifest.payload"]
    if args.signature:
        outputs.append(args.output / "manifest.sig")
    existing = [path for path in outputs if path.exists()]
    if existing and not args.force:
        raise SystemExit(f"refusing to overwrite package files: {existing}")
    args.output.mkdir(parents=True, exist_ok=True)

    digest = hashlib.sha256(image).hexdigest()
    manifest: dict[str, object] = {
        "format_version": 1,
        "board_id": BOARD_ID,
        "partition_id": PARTITION_ID,
        "partition_revision": 1,
        "app_version": app_version,
        "secure_version": secure_version,
        "image_size": len(image),
        "image_sha256": digest,
        "signature": {
            "algorithm": args.signature_algorithm,
            "file": "manifest.sig" if args.signature else None,
            "verified": False,
        },
    }
    (args.output / "firmware.bin").write_bytes(image)
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (args.output / "manifest.payload").write_text(canonical_payload(manifest), encoding="utf-8")
    if args.signature:
        shutil.copyfile(args.signature, args.output / "manifest.sig")
    print(f"Packaged {len(image)} bytes for {app_version}; device-side signature verification is required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
