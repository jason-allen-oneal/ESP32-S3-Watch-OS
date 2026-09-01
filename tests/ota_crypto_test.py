#!/usr/bin/env python3
"""Cross-language OTA payload and P-256 fixture checks."""

from __future__ import annotations

import importlib.util
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)


PROJECT = pathlib.Path(__file__).resolve().parents[1]
PRIVATE_SCALAR = int(
    # Public, test-only fixture. Never use this scalar for release signing.
    "1DCE8D2EC6184CCA7B2F1B33C1AFCE341B7B226B2866A43E79BDCE7D5E28D2D9", 16
)
P256_ORDER = int(
    "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16
)


def load_packager():
    source = PROJECT / "scripts" / "package-update.py"
    spec = importlib.util.spec_from_file_location("nightglass_package_update", source)
    if spec is None or spec.loader is None:
        raise AssertionError("unable to load package-update.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rejected(public_key, signature: bytes, payload: bytes) -> bool:
    try:
        public_key.verify(signature, payload, ec.ECDSA(hashes.SHA256()))
    except (InvalidSignature, ValueError):
        return True
    return False


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            f"usage: {sys.argv[0]} <update-manifest-test-binary> <verifier-test-binary>"
        )

    manifest = {
        "format_version": 1,
        "board_id": "nightglass-waveshare-esp32s3r8-2.06-v1.0",
        "partition_id": "nightglass-32m-r1",
        "partition_revision": 1,
        "app_version": "v0.3.0",
        "secure_version": 2,
        "image_size": 1024,
        "image_sha256": "ab" * 32,
    }
    python_payload = load_packager().canonical_payload(manifest).encode("ascii")
    cpp_payload = subprocess.run(
        [sys.argv[1], "--print-payload"], check=True, capture_output=True
    ).stdout
    assert python_payload == cpp_payload

    private_key = ec.derive_private_key(PRIVATE_SCALAR, ec.SECP256R1())
    public_key = private_key.public_key()
    public_point = public_key.public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint,
    )
    assert public_point.hex() == (
        "04f193c27046bd5086423c957dc4371fe454b0714ffdc3e1654d4cb6d22277cb30"
        "f6344f81746bbce9120dd498bff9c1adb6b9133fa23538aa4ebec773f755c406"
    )

    signature = private_key.sign(python_payload, ec.ECDSA(hashes.SHA256()))
    public_key.verify(signature, python_payload, ec.ECDSA(hashes.SHA256()))
    assert rejected(public_key, signature, python_payload[:-2] + b"0\n")

    wrong_key = ec.generate_private_key(ec.SECP256R1()).public_key()
    assert rejected(wrong_key, signature, python_payload)
    flipped = bytearray(signature)
    flipped[-1] ^= 1
    assert rejected(public_key, bytes(flipped), python_payload)
    assert rejected(public_key, signature[:-1], python_payload)
    assert rejected(public_key, signature + b"\0", python_payload)

    r_value, s_value = decode_dss_signature(signature)
    low_s = min(s_value, P256_ORDER - s_value)
    high_s = P256_ORDER - low_s
    public_key.verify(
        encode_dss_signature(r_value, low_s), python_payload, ec.ECDSA(hashes.SHA256())
    )
    public_key.verify(
        encode_dss_signature(r_value, high_s), python_payload, ec.ECDSA(hashes.SHA256())
    )

    try:
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), b"\x04" + b"\0" * 64)
    except ValueError:
        pass
    else:
        raise AssertionError("off-curve SEC1 point accepted")

    # Exercise the real packager, then feed its exact emitted artifacts into
    # the actual C++ mbedTLS verifier used by firmware.
    image = bytearray(512)
    image[0] = 0xE9
    struct.pack_into("<II", image, 0x20, 0xABCD5432, 2)
    image[0x30 : 0x30 + len(b"v0.3.0")] = b"v0.3.0"
    image[0x50 : 0x50 + len(b"nightglass")] = b"nightglass"
    packaged_manifest = dict(manifest)
    packaged_manifest["image_size"] = len(image)
    packaged_manifest["image_sha256"] = hashlib.sha256(image).hexdigest()
    packaged_payload = load_packager().canonical_payload(packaged_manifest).encode("ascii")
    packaged_signature = private_key.sign(
        packaged_payload, ec.ECDSA(hashes.SHA256())
    )
    with tempfile.TemporaryDirectory(prefix="nightglass-ota-test-") as temporary:
        root = pathlib.Path(temporary)
        image_path = root / "nightglass.bin"
        signature_path = root / "manifest.sig"
        output_path = root / "package"
        image_path.write_bytes(image)
        signature_path.write_bytes(packaged_signature)
        subprocess.run(
            [
                sys.executable,
                str(PROJECT / "scripts" / "package-update.py"),
                str(image_path),
                str(output_path),
                "--signature",
                str(signature_path),
                "--signature-algorithm",
                "ecdsa-p256-sha256-der",
            ],
            check=True,
            capture_output=True,
        )
        assert (output_path / "manifest.payload").read_bytes() == packaged_payload
        subprocess.run(
            [
                sys.argv[2],
                "--verify-files",
                str(output_path / "manifest.payload"),
                str(output_path / "manifest.sig"),
                public_point.hex(),
            ],
            check=True,
        )

    print("Nightglass OTA crypto and packager fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
