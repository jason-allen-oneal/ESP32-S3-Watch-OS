#!/usr/bin/env python3
"""Fail closed unless partitions.csv matches the reviewed Nightglass OTA layout."""

from __future__ import annotations

import csv
import pathlib
import sys


EXPECTED = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x600000),
    "ota_1": ("app", "ota_1", 0x620000, 0x600000),
    "coredump": ("data", "coredump", 0xC20000, 0x80000),
    "assets": ("data", "littlefs", 0xCA0000, 0x1360000),
}
FLASH_SIZE = 32 * 1024 * 1024


def main() -> int:
    source = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "partitions.csv")
    rows: dict[str, tuple[str, str, int, int]] = {}
    with source.open(newline="", encoding="utf-8") as handle:
        filtered = (line for line in handle if line.strip() and not line.lstrip().startswith("#"))
        for row in csv.reader(filtered):
            if len(row) < 5:
                raise SystemExit(f"malformed partition row: {row!r}")
            name, kind, subtype, offset, size = (field.strip() for field in row[:5])
            if name in rows:
                raise SystemExit(f"duplicate partition: {name}")
            rows[name] = (kind, subtype, int(offset, 0), int(size, 0))

    if rows != EXPECTED:
        missing = sorted(EXPECTED.keys() - rows.keys())
        extra = sorted(rows.keys() - EXPECTED.keys())
        mismatched = sorted(name for name in rows.keys() & EXPECTED.keys() if rows[name] != EXPECTED[name])
        raise SystemExit(
            f"partition layout differs from reviewed revision 1: "
            f"missing={missing} extra={extra} mismatched={mismatched}"
        )

    ordered = sorted((offset, offset + size, name) for name, (_, _, offset, size) in rows.items())
    for previous, current in zip(ordered, ordered[1:]):
        if previous[1] > current[0]:
            raise SystemExit(f"partition overlap: {previous[2]} and {current[2]}")
    if ordered[-1][1] != FLASH_SIZE:
        raise SystemExit("partition layout does not end at the 32 MiB flash boundary")
    print("Nightglass partition revision 1 verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
