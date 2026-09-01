#!/usr/bin/env python3
"""Normalize an ECDSA P-256 DER signature to canonical low-S form."""

from __future__ import annotations

import argparse
import pathlib

from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)


P256_ORDER = int("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551", 16)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("signature", type=pathlib.Path)
    args = parser.parse_args()
    raw = args.signature.read_bytes()
    r, s = decode_dss_signature(raw)
    if not 1 <= r < P256_ORDER or not 1 <= s < P256_ORDER:
        raise SystemExit("signature scalar is outside the P-256 group")
    if s > P256_ORDER // 2:
        s = P256_ORDER - s
    normalized = encode_dss_signature(r, s)
    args.signature.write_bytes(normalized)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

