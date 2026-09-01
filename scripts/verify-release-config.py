#!/usr/bin/env python3
"""Verify Nightglass safety-critical settings in an ESP-IDF config artifact."""

from __future__ import annotations

import pathlib
import re
import sys


REQUIRED_ENABLED = (
    "CONFIG_ESP_COREDUMP_ENABLE_TO_NONE",
    "CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION",
)
REQUIRED_DISABLED = (
    "CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST",
    "CONFIG_NIGHTGLASS_OTA_ALLOW_UNSIGNED_DEVELOPMENT",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_UART",
)
REQUIRED_VALUES = {
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "98304",
    "CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "8",
    # Until production key custody and provisioning are separately approved,
    # release artifacts must remain dormant/fail-closed.
    "CONFIG_NIGHTGLASS_OTA_P256_PUBLIC_KEY_HEX": '""',
}


def parse_config(source: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(
        source.read_text(encoding="utf-8").splitlines(), start=1
    ):
        line = raw_line.strip()
        match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
        if match is None:
            match = re.fullmatch(r"#define\s+(CONFIG_[A-Z0-9_]+)(?:\s+(.*))?", line)
        if match is None:
            match = re.fullmatch(r"#\s+(CONFIG_[A-Z0-9_]+)\s+is not set", line)
            if match is not None:
                name, value = match.group(1), "n"
            else:
                continue
        else:
            name, value = match.group(1), (match.group(2) or "1").strip()
        if name in values and values[name] != value:
            raise ValueError(f"conflicting {name} definitions near line {line_number}")
        values[name] = value
    return values


def verify(values: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in REQUIRED_ENABLED:
        if values.get(name) not in {"y", "1"}:
            failures.append(f"{name} must resolve enabled")
    for name in REQUIRED_DISABLED:
        if values.get(name, "n") not in {"n", "0"}:
            failures.append(f"{name} must resolve disabled")
    for name, expected in REQUIRED_VALUES.items():
        if values.get(name) != expected:
            failures.append(f"{name} must resolve to {expected}")
    return failures


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <sdkconfig-or-sdkconfig.h>")
    source = pathlib.Path(sys.argv[1])
    try:
        values = parse_config(source)
        failures = verify(values)
    except (OSError, UnicodeError, ValueError) as error:
        raise SystemExit(f"unable to verify release config {source}: {error}") from error
    if failures:
        raise SystemExit("unsafe Nightglass release config:\n- " + "\n- ".join(failures))
    print(f"Nightglass release config verified: {source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
