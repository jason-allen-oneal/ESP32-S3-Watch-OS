#!/usr/bin/env python3
"""Guard the hardware-task crash and gyro power-cycle fixes."""

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/nightglass_services/src/hardware.cpp").read_text()
DEFAULTS = (ROOT / "sdkconfig.defaults").read_text()


def constant(name: str) -> int:
    match = re.search(rf"constexpr\s+[^;=]+\s+{name}\s*=\s*([0-9']+)", SOURCE)
    assert match, f"missing {name}"
    return int(match.group(1).replace("'", ""))


def main() -> None:
    assert constant("kHardwareTaskStackBytes") >= 8192
    assert constant("kHardwareMinimumStackReserveBytes") >= 2048
    assert constant("kGyroWarningIntervalUs") >= 5_000_000
    assert "if (!retain_calibration) gyro_processor.reset();" in SOURCE
    assert "uxTaskGetStackHighWaterMark(nullptr)" in SOURCE
    assert "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y" in DEFAULTS
    assert "CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y" in DEFAULTS
    print("Nightglass hardware stack and gyro power-cycle contract passed")


if __name__ == "__main__":
    main()
