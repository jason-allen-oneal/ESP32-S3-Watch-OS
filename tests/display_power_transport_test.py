#!/usr/bin/env python3
"""Run production display-power and touch-constructor code with hardware doubles.

The SH8601 public display-on/off boundary runs the installed driver's real
tx_param and disp_on_off bodies. Golden wire words are independent constants,
not computed with Nightglass's helper. This verifies submitted transactions and
requested delays; it does not establish physical panel or touch behavior.

Use --source PATH for the same display regression against an earlier board.cpp.
An earlier source without create_touch_controller runs display checks only.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path,
                    default=ROOT / "components/nightglass_bsp/src/board.cpp")
parser.add_argument("--constructor-only", action="store_true")
args = parser.parse_args()
source = args.source.read_text()
driver = (ROOT / "managed_components/waveshare__esp_lcd_sh8601/esp_lcd_sh8601.c").read_text()
touch_header = (ROOT / "managed_components/espressif__esp_lcd_touch_ft5x06/include/esp_lcd_touch_ft5x06.h").read_text()
board_header = (ROOT / "managed_components/waveshare__esp32_s3_touch_amoled_2_06/include/bsp/esp32_s3_touch_amoled_2_06.h").read_text()
display_header = (ROOT / "managed_components/waveshare__esp32_s3_touch_amoled_2_06/include/bsp/display.h").read_text()
sdkconfig = (ROOT / "sdkconfig").read_text()


def function(text: str, signature: str) -> str:
    # Require an opening body, not an earlier forward declaration.
    match = re.search(re.escape(signature) + r"[^;{}]*\{", text)
    if match is None:
        raise AssertionError(f"Missing production function: {signature}")
    opening = match.end() - 1
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def define(text: str, name: str) -> str:
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+[^\n]+", text, re.M)
    if match is None:
        raise AssertionError(f"Missing installed definition: {name}")
    return match[0]


def config_value(name: str) -> int:
    return int(re.search(r"^" + re.escape(name) + r"=(\d+)$", sdkconfig, re.M)[1])


has_constructor = "esp_err_t create_touch_controller()" in source
constructor = function(source, "esp_err_t create_touch_controller(") if has_constructor else ""
timeout = re.search(r"constexpr int kTouchTransactionTimeoutMs = (\d+);", source)
production = "\n".join(function(source, f"nightglass::core::Status Board::{name}(")
                       for name in ("sleep_display", "wake_display", "set_brightness"))
wire_driver = "\n".join(function(driver, name) for name in (
    "static esp_err_t tx_param(", "static esp_err_t panel_sh8601_disp_on_off("))
definitions = "\n".join([
    define(driver, "LCD_OPCODE_WRITE_CMD"),
    define(touch_header, "ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS"),
    define(board_header, "BSP_LCD_TOUCH_RST"),
    define(board_header, "BSP_LCD_TOUCH_INT"),
    define(display_header, "BSP_LCD_H_RES"),
    define(display_header, "BSP_LCD_V_RES"),
    f"constexpr int CONFIG_BSP_I2C_CLK_SPEED_HZ = {config_value('CONFIG_BSP_I2C_CLK_SPEED_HZ')};",
    f"constexpr int kTickMs = 1000 / {config_value('CONFIG_FREERTOS_HZ')};",
    f"constexpr int kTouchTransactionTimeoutMs = {timeout[1] if timeout else 0};",
    f"#define HAS_TOUCH_CONSTRUCTOR {int(has_constructor)}",
    f"#define CONSTRUCTOR_ONLY {int(args.constructor_only)}",
])

PRELUDE = r'''
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "nightglass/bsp/board.hpp"
#include "nightglass/bsp/sh8601.hpp"

using esp_err_t = int;
using TickType_t = unsigned int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 0x107;
constexpr int GPIO_NUM_NC = -1, GPIO_NUM_9 = 9, GPIO_NUM_38 = 38;
constexpr int GPIO_MODE_OUTPUT = 2;
constexpr int LCD_CMD_DISPOFF = 0x28, LCD_CMD_DISPON = 0x29;
#define ESP_LOGI(...) do {} while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { const int e = (expr); if (e != ESP_OK) return e; } while (0)
#define __containerof(ptr, type, member) reinterpret_cast<type *>(reinterpret_cast<char *>(ptr) - offsetof(type, member))

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
struct Io { int identity; } panel_io_storage{1}, touch_io_storage{2};
using esp_lcd_panel_io_handle_t = Io *;
struct esp_lcd_panel_t {};
using esp_lcd_panel_handle_t = esp_lcd_panel_t *;
struct sh8601_panel_t {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    struct { unsigned int use_qspi_interface : 1; } flags;
} panel_storage{{}, &panel_io_storage, {1}};
esp_lcd_panel_handle_t panel_handle = &panel_storage.base;
esp_lcd_panel_io_handle_t panel_io = &panel_io_storage;
nightglass::bsp::Board *observed_board = nullptr;
enum class Kind { wire, delay, brightness };
struct Event {
    Kind kind;
    std::uint32_t value;
    bool cached_sleep;
};
std::vector<Event> events;
std::deque<esp_err_t> wire_results;
esp_err_t brightness_result = ESP_OK;
bool cached_sleep() { return observed_board && observed_board->display_sleep_requested(); }
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command,
                                   const void *data, std::size_t size) {
    require(io == &panel_io_storage, "display command routed to wrong IO");
    require(data == nullptr && size == 0, "power command acquired unexpected parameters");
    events.push_back({Kind::wire, static_cast<std::uint32_t>(command), cached_sleep()});
    if (wire_results.empty()) return ESP_OK;
    const auto result = wire_results.front();
    wire_results.pop_front();
    return result;
}
esp_err_t bsp_display_brightness_set(int percent) {
    events.push_back({Kind::brightness, static_cast<std::uint32_t>(percent), cached_sleep()});
    return brightness_result;
}
'''

HARDWARE = r'''
std::vector<std::string> init_events;
bool capture_touch_delays = false;
TickType_t pdMS_TO_TICKS(int milliseconds) { return milliseconds / kTickMs; }
void vTaskDelay(TickType_t ticks) {
    events.push_back({Kind::delay, ticks * kTickMs, cached_sleep()});
    if (capture_touch_delays) init_events.push_back("delay_" + std::to_string(ticks * kTickMs));
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on) {
    require(panel == &panel_storage.base, "display power used wrong panel handle");
    return panel_sh8601_disp_on_off(panel, on);
}

// Field order follows pinned IDF esp_lcd_io_i2c.h, including default-zero fields.
struct esp_lcd_panel_io_i2c_config_t {
    std::uint32_t dev_addr = 0;
    void *on_color_trans_done = nullptr;
    void *user_ctx = nullptr;
    std::size_t control_phase_bytes = 0;
    unsigned int dc_bit_offset = 0;
    int lcd_cmd_bits = 0;
    int lcd_param_bits = 0;
    struct { unsigned int dc_low_on_data : 1; unsigned int disable_control_phase : 1; } flags{};
    std::uint32_t scl_speed_hz = 0;
    int transaction_timeout_ms = 0;
};
struct esp_lcd_touch_config_t {
    int x_max = 0, y_max = 0, rst_gpio_num = 0, int_gpio_num = 0;
    struct { unsigned int reset : 1; unsigned int interrupt : 1; } levels{};
    struct { unsigned int swap_xy : 1; unsigned int mirror_x : 1; unsigned int mirror_y : 1; } flags{};
};
struct Touch { esp_lcd_touch_config_t config; } touch_storage;
using esp_lcd_touch_handle_t = Touch *;
esp_lcd_touch_handle_t touch_handle = nullptr;
int bus_storage;
i2c_master_bus_handle_t i2c_bus = &bus_storage;
esp_lcd_panel_io_i2c_config_t captured_io_config{};
esp_lcd_touch_config_t captured_touch_config{};
esp_err_t bus_result = ESP_OK, io_result = ESP_OK, touch_result = ESP_OK;
esp_err_t gpio_config_result = ESP_OK;
std::deque<esp_err_t> gpio_level_results;
struct gpio_config_t { std::uint64_t pin_bit_mask; int mode; };
esp_err_t gpio_config(const gpio_config_t *config) {
    init_events.push_back("gpio_config");
    require(config->pin_bit_mask == (1ULL << GPIO_NUM_9) && config->mode == GPIO_MODE_OUTPUT,
            "startup reset configured the wrong pin or direction");
    return gpio_config_result;
}
esp_err_t gpio_set_level(int pin, int level) {
    require(pin == GPIO_NUM_9 && (level == 0 || level == 1), "startup reset drove the wrong pin/level");
    init_events.push_back(level ? "reset_high" : "reset_low");
    if (gpio_level_results.empty()) return ESP_OK;
    const auto result = gpio_level_results.front();
    gpio_level_results.pop_front();
    return result;
}
esp_err_t gpio_reset_pin(int pin) {
    require(pin == GPIO_NUM_9, "startup cleanup reset an unrelated pin");
    init_events.push_back("gpio_reset");
    return ESP_OK;
}
esp_err_t bsp_i2c_init() { init_events.push_back("bus_init"); return bus_result; }
i2c_master_bus_handle_t bsp_i2c_get_handle() { return i2c_bus; }
esp_err_t esp_lcd_new_panel_io_i2c(i2c_master_bus_handle_t bus,
                                  const esp_lcd_panel_io_i2c_config_t *config,
                                  esp_lcd_panel_io_handle_t *out) {
    init_events.push_back("io_create");
    require(bus == i2c_bus, "touch controller used a private or wrong I2C bus");
    require(out && *out == nullptr, "touch IO output was not initialized empty");
    captured_io_config = *config;
    if (io_result == ESP_OK) *out = &touch_io_storage;
    return io_result;
}
esp_err_t esp_lcd_touch_new_i2c_ft5x06(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_touch_config_t *config,
                                     esp_lcd_touch_handle_t *out) {
    init_events.push_back("touch_create");
    require(io == &touch_io_storage, "touch constructor received wrong IO");
    require(out == &touch_handle, "touch constructor did not own the expected output");
    captured_touch_config = *config;
    if (touch_result == ESP_OK) { touch_storage.config = *config; *out = &touch_storage; }
    return touch_result;
}
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) {
    require(io == &touch_io_storage, "cleanup deleted an unrelated IO");
    init_events.push_back("io_delete");
    return ESP_OK;
}
'''

CHECKS = r'''
using nightglass::bsp::Board;
using nightglass::core::StatusCode;
constexpr std::uint32_t DISPOFF = 0x02002800, SLPIN = 0x02001000;
constexpr std::uint32_t SLEEPOUT = 0x02001100, DISPON = 0x02002900;

void reset_hardware(Board &board) {
    observed_board = &board;
    panel_io = &panel_io_storage;
    panel_handle = &panel_storage.base;
    events.clear();
    wire_results.clear();
    brightness_result = ESP_OK;
}
void expect_events(std::initializer_list<Event> expected, const char *context) {
    require(events.size() == expected.size(), std::string(context) + ": unexpected operation count");
    std::size_t index = 0;
    for (const auto &want : expected) {
        const auto &got = events[index];
        const auto prefix = std::string(context) + " operation " + std::to_string(index);
        require(got.kind == want.kind, prefix + ": wrong operation/order");
        if (want.kind == Kind::delay) {
            require(got.value >= want.value && got.value <= want.value + kTickMs,
                    prefix + ": required delay was lost or excessive");
        } else {
            require(got.value == want.value, prefix + ": wrong QSPI wire word or brightness");
        }
        require(got.cached_sleep == want.cached_sleep, prefix + ": cached sleep changed prematurely");
        ++index;
    }
}
void put_to_sleep(Board &board) {
    require(board.sleep_display().is_ok(), "test setup could not submit sleep");
    require(board.display_sleep_requested(), "successful sleep was not cached");
    events.clear();
}
void display_checks() {
    require(kTickMs == 10, "test delay model expects the pinned 100Hz scheduler");
    {
        Board board; reset_hardware(board);
        require(board.wake_display().is_ok() && events.empty(), "already-awake wake was not a no-op");
        require(board.sleep_display().is_ok(), "sleep command sequence failed");
        expect_events({{Kind::wire, DISPOFF, false}, {Kind::delay, 10, false},
                       {Kind::wire, SLPIN, false}, {Kind::delay, 5, false}}, "sleep");
        require(board.display_sleep_requested(), "successful sleep state not committed");
        events.clear();
        require(board.sleep_display().is_ok() && events.empty(), "repeated sleep was not a no-op");
        require(board.wake_display().is_ok(), "wake command sequence failed");
        expect_events({{Kind::wire, SLEEPOUT, true}, {Kind::delay, 120, true},
                       {Kind::wire, DISPON, true}, {Kind::brightness, 30, false}}, "wake");
        require(!board.display_sleep_requested(), "successful wake retained sleep state");
        events.clear();
        require(board.wake_display().is_ok() && events.empty(), "repeated wake was not a no-op");
    }
    {
        Board board; reset_hardware(board); panel_io = nullptr;
        require(board.sleep_display().code == StatusCode::invalid_state, "sleep accepted missing IO");
        require(events.empty() && !board.display_sleep_requested(), "missing IO mutated awake state");
        panel_io = &panel_io_storage; put_to_sleep(board); panel_io = nullptr;
        require(board.wake_display().code == StatusCode::invalid_state, "wake accepted missing IO");
        require(events.empty() && board.display_sleep_requested(), "missing IO mutated sleeping state");
    }
    {
        Board board; reset_hardware(board); wire_results = {ESP_ERR_TIMEOUT};
        require(board.sleep_display().code == StatusCode::io_error, "DISPOFF error swallowed");
        expect_events({{Kind::wire, DISPOFF, false}}, "failed DISPOFF");
        require(!board.display_sleep_requested(), "failed DISPOFF cached sleep");
        events.clear(); put_to_sleep(board);
    }
    for (const auto restore_result : {ESP_OK, ESP_FAIL}) {
        Board board; reset_hardware(board);
        wire_results = {ESP_OK, ESP_ERR_TIMEOUT, restore_result};
        require(board.sleep_display().code == StatusCode::io_error, "SLPIN error swallowed");
        expect_events({{Kind::wire, DISPOFF, false}, {Kind::delay, 10, false},
                       {Kind::wire, SLPIN, false}, {Kind::wire, DISPON, false}}, "failed SLPIN restoration");
        require(!board.display_sleep_requested(), "failed SLPIN cached successful sleep");
        // A failed restoration is not claimed as physical scan-out restoration.
    }
    {
        Board board; reset_hardware(board); put_to_sleep(board);
        wire_results = {ESP_ERR_TIMEOUT};
        require(board.wake_display().code == StatusCode::io_error, "SLEEPOUT error swallowed");
        expect_events({{Kind::wire, SLEEPOUT, true}}, "failed SLEEPOUT");
        require(board.display_sleep_requested(), "failed SLEEPOUT cached wake");
        events.clear(); require(board.wake_display().is_ok(), "SLEEPOUT failure was not retryable");
    }
    {
        Board board; reset_hardware(board); put_to_sleep(board);
        wire_results = {ESP_OK, ESP_FAIL};
        require(board.wake_display().code == StatusCode::io_error, "DISPON error swallowed");
        expect_events({{Kind::wire, SLEEPOUT, true}, {Kind::delay, 120, true},
                       {Kind::wire, DISPON, true}}, "failed DISPON");
        require(board.display_sleep_requested(), "failed DISPON cached wake");
        events.clear(); require(board.wake_display().is_ok(), "DISPON failure was not retryable");
        expect_events({{Kind::wire, SLEEPOUT, true}, {Kind::delay, 120, true},
                       {Kind::wire, DISPON, true}, {Kind::brightness, 30, false}}, "DISPON retry");
    }
    {
        Board board; reset_hardware(board); put_to_sleep(board);
        brightness_result = ESP_FAIL;
        require(board.set_brightness(42).code == StatusCode::io_error, "brightness restoration failure swallowed");
        expect_events({{Kind::wire, SLEEPOUT, true}, {Kind::delay, 120, true},
                       {Kind::wire, DISPON, true}, {Kind::brightness, 42, false}}, "failed brightness restoration");
        require(!board.display_sleep_requested(), "brightness failure falsely cached sleeping panel");
        events.clear(); brightness_result = ESP_OK;
        require(board.set_brightness(42).is_ok(), "brightness request was not retryable");
        expect_events({{Kind::brightness, 42, false}}, "brightness-only retry");
        events.clear();
        require(board.set_brightness(0).is_ok(), "zero brightness did not request sleep");
        require(board.display_sleep_requested(), "zero brightness did not cache successful sleep");
        events.clear(); require(board.wake_display().is_ok(), "wake after zero brightness failed");
        expect_events({{Kind::wire, SLEEPOUT, true}, {Kind::delay, 120, true},
                       {Kind::wire, DISPON, true}, {Kind::brightness, 42, false}}, "saved brightness");
        events.clear();
        require(board.set_brightness(101).code == StatusCode::invalid_state && events.empty(),
                "invalid brightness submitted hardware work");
    }
}

#if HAS_TOUCH_CONSTRUCTOR
void reset_touch_init() {
    init_events.clear(); touch_handle = nullptr;
    captured_io_config = {}; captured_touch_config = {};
    bus_result = io_result = touch_result = ESP_OK;
    gpio_config_result = ESP_OK; gpio_level_results.clear();
    observed_board = nullptr; events.clear(); capture_touch_delays = true;
}
void touch_constructor_checks() {
    reset_touch_init();
    require(nightglass::bsp::create_touch_controller() == ESP_OK, "touch creation failed");
    const std::vector<std::string> startup{"bus_init", "io_create", "gpio_config", "reset_low",
                                         "delay_20", "reset_high", "delay_100", "touch_create"};
    require(init_events == startup,
            "touch creation duplicated/reordered initialization");
    require(touch_handle == &touch_storage, "touch output not retained");
    require(touch_handle->config.rst_gpio_num == GPIO_NUM_9,
            "successful touch handle did not regain real reset pin ownership");
    const auto &io = captured_io_config;
    require(io.dev_addr == 0x38 && io.scl_speed_hz == CONFIG_BSP_I2C_CLK_SPEED_HZ,
            "touch address or existing bus clock changed");
    require(io.transaction_timeout_ms == 50, "touch transaction timeout is not bounded to 50ms");
    require(io.control_phase_bytes == 1 && io.dc_bit_offset == 0 && io.lcd_cmd_bits == 8 &&
            io.lcd_param_bits == 0 && io.flags.dc_low_on_data == 0 && io.flags.disable_control_phase == 1 &&
            io.on_color_trans_done == nullptr && io.user_ctx == nullptr,
            "touch IO differs from installed BSP/FT5x06 defaults");
    const auto &touch = captured_touch_config;
    require(touch.x_max == 410 && touch.y_max == 502 && touch.rst_gpio_num == GPIO_NUM_NC && touch.int_gpio_num == 38,
            "touch geometry or independent reset/IRQ wiring changed");
    require(touch.levels.reset == 0 && touch.levels.interrupt == 0 && touch.flags.swap_xy == 0 &&
            touch.flags.mirror_x == 0 && touch.flags.mirror_y == 0,
            "touch polarity or coordinate flags changed");

    reset_touch_init(); bus_result = ESP_ERR_TIMEOUT;
    require(nightglass::bsp::create_touch_controller() == ESP_ERR_TIMEOUT, "I2C prerequisite error changed");
    require(init_events == std::vector<std::string>{"bus_init"} && !touch_handle,
            "touch IO/driver created after failed bus prerequisite");
    reset_touch_init(); io_result = ESP_FAIL;
    require(nightglass::bsp::create_touch_controller() == ESP_FAIL, "touch IO failure swallowed");
    require(init_events == std::vector<std::string>{"bus_init", "io_create"} && !touch_handle,
            "touch driver created or unowned IO deleted after IO allocation failure");
    reset_touch_init(); touch_result = ESP_ERR_TIMEOUT;
    require(nightglass::bsp::create_touch_controller() == ESP_ERR_TIMEOUT, "touch constructor error changed");
    auto constructor_failure = startup;
    constructor_failure.insert(constructor_failure.end(), {"gpio_reset", "io_delete"});
    require(init_events == constructor_failure && !touch_handle,
            "failed touch constructor did not release its reset pin and IO");

    reset_touch_init(); gpio_config_result = ESP_FAIL;
    require(nightglass::bsp::create_touch_controller() == ESP_FAIL, "reset configuration error changed");
    require(init_events == std::vector<std::string>{"bus_init", "io_create", "gpio_config", "gpio_reset", "io_delete"}
            && !touch_handle, "reset configuration failure did not stop and clean up");
    reset_touch_init(); gpio_level_results = {ESP_FAIL};
    require(nightglass::bsp::create_touch_controller() == ESP_FAIL, "reset assertion error changed");
    require(init_events == std::vector<std::string>{"bus_init", "io_create", "gpio_config", "reset_low", "gpio_reset", "io_delete"}
            && !touch_handle, "reset assertion failure did not stop and clean up");
    reset_touch_init(); gpio_level_results = {ESP_OK, ESP_FAIL};
    require(nightglass::bsp::create_touch_controller() == ESP_FAIL, "reset release error changed");
    require(init_events == std::vector<std::string>{"bus_init", "io_create", "gpio_config", "reset_low", "delay_20",
                                                  "reset_high", "gpio_reset", "io_delete"}
            && !touch_handle, "reset release failure did not stop and clean up");
}
#endif

int main() {
    try {
        if (!CONSTRUCTOR_ONLY) display_checks();
#if HAS_TOUCH_CONSTRUCTOR
        touch_constructor_checks();
#endif
        std::cout << (CONSTRUCTOR_ONLY ? "Nightglass touch constructor semantics passed\n"
                                      : "Nightglass display power transport / touch constructor semantics passed\n");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
'''

with tempfile.TemporaryDirectory(prefix="nightglass-display-transport-") as temporary:
    directory = Path(temporary)
    (directory / "driver").mkdir()
    (directory / "driver/i2c_master.h").write_text(
        "#pragma once\nusing i2c_master_bus_handle_t = void *;\n")
    unit = directory / "display_power_transport_test.cpp"
    unit.write_text(PRELUDE + "\n" + definitions + "\n" + wire_driver + "\n" + HARDWARE +
                    "\nnamespace nightglass::bsp {\n" + constructor + "\n" + production +
                    "\n}\n" + CHECKS)
    binary = directory / "display_power_transport_test"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(directory), "-I", str(ROOT / "components/nightglass_bsp/include"),
                    "-I", str(ROOT / "components/nightglass_core/include"), str(unit), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
