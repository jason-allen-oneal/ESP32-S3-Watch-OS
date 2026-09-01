#!/usr/bin/env python3
"""Verify a complete signed Nightglass OTA package without private material."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature


P256_ORDER = int("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16)
ALGORITHM = "ecdsa-p256-sha256-der"


def canonical_payload(manifest: dict[str, object]) -> bytes:
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
    ).encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=pathlib.Path)
    parser.add_argument(
        "--public-key",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "config" / "ota-public-key.hex",
    )
    args = parser.parse_args()

    manifest = json.loads((args.package / "manifest.json").read_text(encoding="utf-8"))
    payload = (args.package / "manifest.payload").read_bytes()
    firmware = (args.package / "firmware.bin").read_bytes()
    signature = (args.package / "manifest.sig").read_bytes()
    if payload != canonical_payload(manifest):
        raise SystemExit("manifest payload is not canonical")
    if len(firmware) != manifest["image_size"]:
        raise SystemExit("firmware size does not match manifest")
    if hashlib.sha256(firmware).hexdigest() != manifest["image_sha256"]:
        raise SystemExit("firmware SHA-256 does not match manifest")
    signature_metadata = manifest.get("signature")
    if not isinstance(signature_metadata, dict) or \
       signature_metadata.get("algorithm") != ALGORITHM or \
       signature_metadata.get("file") != "manifest.sig":
        raise SystemExit("signature metadata is invalid")

    r, s = decode_dss_signature(signature)
    if not 1 <= r < P256_ORDER or not 1 <= s <= P256_ORDER // 2:
        raise SystemExit("signature is not canonical low-S P-256 DER")
    point = bytes.fromhex(args.public_key.read_text(encoding="ascii").strip())
    public_key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), point)
    try:
        public_key.verify(signature, payload, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature as error:
        raise SystemExit("manifest signature verification failed") from error
    print("Nightglass signed package verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

