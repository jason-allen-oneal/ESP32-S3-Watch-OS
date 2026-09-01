#!/usr/bin/env python3
"""Host coverage for the fail-closed release configuration verifier."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "scripts" / "verify-release-config.py"

GOOD = {
    "CONFIG_ESP_COREDUMP_ENABLE_TO_NONE": "y",
    "CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION": "y",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "98304",
    "CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "8",
}
UNSAFE = (
    ("CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST", "y"),
    ("CONFIG_NIGHTGLASS_OTA_ALLOW_UNSIGNED_DEVELOPMENT", "y"),
    ("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH", "y"),
    ("CONFIG_ESP_COREDUMP_ENABLE_TO_UART", "y"),
)


def run(contents: str, should_pass: bool) -> None:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8") as config:
        config.write(contents)
        config.flush()
        result = subprocess.run(
            [sys.executable, str(VERIFIER), config.name],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    assert (result.returncode == 0) is should_pass


def sdkconfig(overrides: dict[str, str] | None = None) -> str:
    values = dict(GOOD)
    values.update(overrides or {})
    return "\n".join(f"{name}={value}" for name, value in values.items()) + "\n"


def main() -> int:
    run(sdkconfig(), True)
    header = "\n".join(
        f"#define {name} {1 if value == 'y' else value}" for name, value in GOOD.items()
    )
    run(header + "\n", True)
    for name in GOOD:
        reduced = dict(GOOD)
        reduced.pop(name)
        run("\n".join(f"{key}={value}" for key, value in reduced.items()) + "\n", False)
    for name, value in UNSAFE:
        run(sdkconfig({name: value}), False)
    run(sdkconfig({"CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "65536"}), False)
    run(sdkconfig({"CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "32"}), False)
    print("Nightglass release config tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
