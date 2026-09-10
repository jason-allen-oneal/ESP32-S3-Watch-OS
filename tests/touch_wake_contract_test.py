#!/usr/bin/env python3
"""Guard the touch wake and liveness regression fixes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
power = (ROOT / "components/nightglass_services/src/power.cpp").read_text()
board = (ROOT / "components/nightglass_bsp/src/board.cpp").read_text()
shell = (ROOT / "components/nightglass_ui/src/shell.cpp").read_text()
update = (ROOT / "components/nightglass_update/src/service.cpp").read_text()

# Automatic light sleep cannot be re-enabled until GPIO38 has a tested,
# race-safe edge/level hand-off. Explicit light sleep remains bracketed below.
assert "config.light_sleep_enable = false;" in power
assert "config.light_sleep_enable = true;" not in power
assert "gpio_wakeup_enable(kTouchInterruptGpio, GPIO_INTR_LOW_LEVEL)" in power
assert "gpio_set_intr_type(kTouchInterruptGpio, GPIO_INTR_NEGEDGE)" in power

# Initialization alone is not touch health. A real controller read and a
# registered LVGL/GPIO interrupt are required, and liveness is refreshed.
assert "read_touch_controller(initial_touch_point, initial_touch_count)" in board
assert "touch_irq_registered" in board
assert "record_touch_read_success();" in board
assert "touch_liveness" in board
assert '"BSP touch input active"' not in board
assert "TOUCH_LIVENESS controller=1" in board
assert "configure_ft3168(touch_configuration, touch_device_id, touch_power_mode)" in board
assert board.index("configure_ft3168(touch_configuration") < board.index("touch_input = lv_indev_create()")

# The first contact on a blank display is wake-only, but the input must keep
# polling until release instead of remaining permanently wedged.
wake_only = shell.index("before.wake_touch_pending")
resume = shell.index("lv_timer_resume(read_timer);", wake_only)
wait_release = shell.index("lv_indev_wait_release(touch_input_);", wake_only)
assert resume < wait_release

# OTA acceptance requires recent controller-read evidence, not a stale state.
assert "health_evidence_fresh(now_us, record.last_success_us" in update
assert "kTouchHealthMaximumAgeUs" in update

print("Nightglass touch wake/liveness contract passed")
