#!/usr/bin/env python3
"""Verify the embedded Nightglass application identity used for a release."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


APP_DESCRIPTION_OFFSET = 0x20
APP_DESCRIPTION_MINIMUM = APP_DESCRIPTION_OFFSET + 80
APP_DESCRIPTION_MAGIC = 0xABCD5432


def fixed_text(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("ascii", errors="strict")


def application_identity(image: bytes) -> tuple[str, str, int]:
    if len(image) < APP_DESCRIPTION_MINIMUM or image[0] != 0xE9:
        raise ValueError("not an ESP application image")
    magic, secure_version = struct.unpack_from("<II", image, APP_DESCRIPTION_OFFSET)
    if magic != APP_DESCRIPTION_MAGIC:
        raise ValueError("ESP application descriptor is missing")
    version = fixed_text(image[APP_DESCRIPTION_OFFSET + 16 : APP_DESCRIPTION_OFFSET + 48])
    project = fixed_text(image[APP_DESCRIPTION_OFFSET + 48 : APP_DESCRIPTION_OFFSET + 80])
    return project, version, secure_version


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--secure-version", required=True, type=int)
    args = parser.parse_args()
    try:
        project, version, secure_version = application_identity(args.image.read_bytes())
    except (OSError, UnicodeError, ValueError) as error:
        raise SystemExit(f"application identity verification failed: {error}") from None
    if project != "nightglass":
        raise SystemExit(f"application project is {project!r}, expected 'nightglass'")
    if version != args.version:
        raise SystemExit(f"application version is {version!r}, expected {args.version!r}")
    if secure_version != args.secure_version:
        raise SystemExit(
            f"application secure version is {secure_version}, expected {args.secure_version}"
        )
    print(
        f"Nightglass application identity verified: version={version} "
        f"secure_version={secure_version}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
