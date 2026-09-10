#!/usr/bin/env python3
"""Host coverage for the fail-closed release configuration verifier."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
VERIFIER = ROOT / "scripts" / "verify-release-config.py"
PUBLIC_KEY = (ROOT / "config" / "ota-public-key.hex").read_text(encoding="ascii").strip()

GOOD = {
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "y",
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "8192",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_NONE": "y",
    "CONFIG_LOG_DEFAULT_LEVEL_INFO": "y",
    "CONFIG_LOG_DEFAULT_LEVEL": "3",
    "CONFIG_LOG_MAXIMUM_LEVEL": "3",
    "CONFIG_NIGHTGLASS_OTA_ENABLED": "y",
    "CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION": "y",
    "CONFIG_SPIRAM_FETCH_INSTRUCTIONS": "y",
    "CONFIG_SPIRAM_RODATA": "y",
    "CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM": "y",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "98304",
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "1024",
    "CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "8",
    "CONFIG_NIGHTGLASS_OTA_P256_PUBLIC_KEY_HEX": f'"{PUBLIC_KEY}"',
}
UNSAFE = (
    ("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL", "y"),
    ("CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST", "y"),
    ("CONFIG_NIGHTGLASS_GESTURE_BOOT_TRACE", "y"),
    ("CONFIG_NIGHTGLASS_OTA_ALLOW_UNSIGNED_DEVELOPMENT", "y"),
    ("CONFIG_NIGHTGLASS_OTA_HIL_FORCE_HEALTH_FAILURE", "y"),
    ("CONFIG_NIGHTGLASS_OTA_REQUIRE_PHYSICAL_CONFIRMATION", "y"),
    ("CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK", "y"),
    ("CONFIG_NIGHTGLASS_OTA_P256_PUBLIC_KEY_HEX", '"04deadbeef"'),
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
    run(sdkconfig({"CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "16384"}), False)
    run(sdkconfig({"CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "32"}), False)
    invalid_version = subprocess.run(
        [sys.executable, str(VERIFIER), "/dev/null", "--secure-version", "0"],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    assert invalid_version.returncode != 0
    print("Nightglass release config tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
