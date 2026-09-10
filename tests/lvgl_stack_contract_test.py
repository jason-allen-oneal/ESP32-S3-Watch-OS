#!/usr/bin/env python3
"""Guard the measured LVGL stack-overflow remediation."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
board = (ROOT / "components/nightglass_bsp/src/board.cpp").read_text()
shell = (ROOT / "components/nightglass_ui/src/shell.cpp").read_text()
activity_units = (
    ROOT / "components/nightglass_services/src/activity_units.cpp"
).read_text()
defaults = (ROOT / "sdkconfig.defaults").read_text()

stack_match = re.search(
    r"kLvglTaskStackBytes\s*=\s*(\d+)\s*\*\s*1024", board
)
assert stack_match, "LVGL task stack must be an explicit project policy"
assert int(stack_match.group(1)) >= 16, "LVGL task stack regressed below measured margin"
assert "lvgl_config.task_stack = kLvglTaskStackBytes;" in board
assert "lvgl_config.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;" in board
assert "CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA" in board

reserve_match = re.search(
    r"kLvglMinimumStackReserveBytes\s*=\s*(\d+)", shell
)
assert reserve_match and int(reserve_match.group(1)) >= 4096
assert 'log_lvgl_stack_reserve("navigation");' in shell
assert "uxTaskGetStackHighWaterMark(nullptr)" in shell

float_format = re.compile(r"(?<!%)%(?!%)[-+ #0]*\d*(?:\.\d+)?[aAeEfFgG]")
assert not float_format.search(shell), "LVGL callbacks must not invoke libc float formatting"
assert not float_format.search(activity_units), (
    "home activity formatting must not invoke libc float formatting"
)

assert "CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y" in defaults
assert "CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y" in defaults

print("Nightglass LVGL stack-overflow contract passed")
