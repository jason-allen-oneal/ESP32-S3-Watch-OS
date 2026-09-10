#include "nightglass/bsp/board.hpp"
#include "nightglass/bsp/ft3168.hpp"
#include "nightglass/bsp/sh8601.hpp"
#include "nightglass/bsp/touch_bus_diagnostic.hpp"

#include <algorithm>

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nightglass/core/health.hpp"

namespace nightglass::bsp {

namespace {
Board instance;
constexpr char kTag[] = "nightglass_bsp";
// Route rendering runs from inside LVGL input callbacks and can include libc
// formatting plus a full watch-face rebuild. The esp_lvgl_port default (7168
// bytes) was proven insufficient by the end-of-stack watchpoint on hardware.
constexpr int kLvglTaskStackBytes = 16 * 1024;
constexpr std::uint64_t kTouchProbePeriodUs = 10'000'000;
constexpr std::int64_t kTouchLivenessLogPeriodUs = 60'000'000;
constexpr int kTouchTransactionTimeoutMs = 50;
static_assert(kLvglTaskStackBytes >= 16 * 1024,
              "LVGL task needs measured route-rendering headroom");
static_assert(CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA,
              "External task stacks require cache-safe PSRAM XIP during NVS writes");
lv_display_t *display_handle = nullptr;
lv_indev_t *touch_input = nullptr;
esp_lcd_panel_handle_t panel_handle = nullptr;
esp_lcd_panel_io_handle_t panel_io = nullptr;
esp_lcd_touch_handle_t touch_handle = nullptr;
esp_timer_handle_t touch_probe_timer = nullptr;
std::int64_t last_touch_error_log_us = 0;
std::int64_t last_touch_liveness_log_us = 0;
lv_indev_state_t last_touch_state = LV_INDEV_STATE_RELEASED;
lv_point_t last_touch_point{};
std::uint32_t successful_touch_reads = 0;
std::uint8_t consecutive_touch_errors = 0;
bool touch_degraded = false;
bool touch_irq_registered = false;
// Diagnostic metadata is shared by the LVGL task, hardware task, and GPIO ISR.
// Neither an IRQ nor a request is successful controller-read health evidence.
portMUX_TYPE touch_diagnostic_mux = portMUX_INITIALIZER_UNLOCKED;
std::uint32_t touch_diagnostic_irq_sequence = 0;
bool touch_bus_quiet_requested = false;
bool touch_bus_quiet_request_issued = false;
std::int64_t last_touch_bus_quiet_request_us = 0;
TouchReadEvidence last_touch_read_evidence{};

void request_touch_bus_quiet_diagnostic(esp_err_t probe_result) {
    if (probe_result != ESP_ERR_NOT_FOUND) return;
    const auto now_us = esp_timer_get_time();
    portENTER_CRITICAL(&touch_diagnostic_mux);
    if (!touch_bus_quiet_requested &&
        (!touch_bus_quiet_request_issued ||
         now_us - last_touch_bus_quiet_request_us >= 20'000'000)) {
        touch_bus_quiet_request_issued = true;
        last_touch_bus_quiet_request_us = now_us;
        touch_bus_quiet_requested = true;
    }
    portEXIT_CRITICAL(&touch_diagnostic_mux);
}

esp_err_t create_touch_controller() {
    // Same public primitives and wiring as bsp_touch_new(), but never request
    // an infinite I2C wait while the LVGL input callback holds its mutex.
    const auto bus_result = bsp_i2c_init();
    if (bus_result != ESP_OK) return bus_result;
    // Spell out the vendor macro's values: its C designated-initializer order
    // differs from the pinned IDF struct and cannot be expanded in C++.
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    io_config.transaction_timeout_ms = kTouchTransactionTimeoutMs;
    esp_lcd_panel_io_handle_t io = nullptr;
    const auto io_result = esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &io_config, &io);
    if (io_result != ESP_OK) return io_result;

    // The generic constructor waits only 10 ms after reset, before its first
    // register writes. FT3168 needs at least 70 ms. Own the startup reset here
    // and tell the constructor it is externally managed, so it cannot replace
    // this completed recovery interval with another too-short reset cycle.
    const gpio_config_t reset_config{
        .pin_bit_mask = 1ULL << BSP_LCD_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    auto reset_result = gpio_config(&reset_config);
    if (reset_result == ESP_OK) reset_result = gpio_set_level(BSP_LCD_TOUCH_RST, 0);
    if (reset_result == ESP_OK) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(20)));
        reset_result = gpio_set_level(BSP_LCD_TOUCH_RST, 1);
        if (reset_result == ESP_OK) {
            vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(100)));
        }
    }
    if (reset_result != ESP_OK) {
        gpio_reset_pin(BSP_LCD_TOUCH_RST);
        esp_lcd_panel_io_del(io);
        return reset_result;
    }
    const esp_lcd_touch_config_t config{
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    const auto touch_result = esp_lcd_touch_new_i2c_ft5x06(io, &config, &touch_handle);
    if (touch_result == ESP_OK) {
        // Restore real pin ownership for the existing FT3168 configuration
        // and the driver's eventual cleanup; no runtime reset policy changes.
        touch_handle->config.rst_gpio_num = BSP_LCD_TOUCH_RST;
        ESP_LOGI(kTag, "TOUCH_STARTUP_RESET low_ms=20 recovery_ms=100 generic_reset=external");
    } else {
        gpio_reset_pin(BSP_LCD_TOUCH_RST);
        esp_lcd_panel_io_del(io);
    }
    return touch_result;
}

struct TouchConfigurationIo {
    esp_err_t error = ESP_OK;

    bool set_reset(bool asserted) {
        if (touch_handle->config.rst_gpio_num == GPIO_NUM_NC) {
            error = ESP_ERR_INVALID_STATE;
            return false;
        }
        error = gpio_set_level(touch_handle->config.rst_gpio_num,
                               asserted ? touch_handle->config.levels.reset
                                        : !touch_handle->config.levels.reset);
        return error == ESP_OK;
    }

    void delay_ms(std::uint32_t milliseconds) {
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(milliseconds)));
    }

    bool read(std::uint8_t reg, std::uint8_t &value) {
        error = esp_lcd_panel_io_rx_param(touch_handle->io, reg, &value, 1);
        return error == ESP_OK;
    }

    bool write(std::uint8_t reg, std::uint8_t value) {
        error = esp_lcd_panel_io_tx_param(touch_handle->io, reg, &value, 1);
        return error == ESP_OK;
    }
};

void IRAM_ATTR touch_interrupt(esp_lcd_touch_handle_t) {
    portENTER_CRITICAL_ISR(&touch_diagnostic_mux);
    ++touch_diagnostic_irq_sequence;
    portEXIT_CRITICAL_ISR(&touch_diagnostic_mux);
    if (touch_input) lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, touch_input);
}

void touch_probe_callback(void *) {
    if (touch_input) lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, touch_input);
}

esp_err_t read_touch_controller(esp_lcd_touch_point_data_t &point,
                                std::uint8_t &touch_count) {
    static std::uint32_t attempts = 0;
    static std::uint32_t failures = 0;
    static std::int64_t last_sample_log_us = 0;
    const auto started_us = esp_timer_get_time();
    const auto read_result = esp_lcd_touch_read_data(touch_handle);
    const auto result = read_result == ESP_OK
        ? esp_lcd_touch_get_data(touch_handle, &point, &touch_count, 1) : read_result;
    const auto completed_us = esp_timer_get_time();
    ++attempts;
    if (result != ESP_OK) ++failures;
    if (last_sample_log_us == 0 || completed_us - last_sample_log_us >= 2'000'000) {
        last_sample_log_us = completed_us;
        // Diagnostic reads earn no health credit and never replace the sample
        // result. Probe only after a failed register read to distinguish an
        // address ACK failure from other INVALID_STATE transport failures.
        std::uint8_t mode = 0xff;
        const auto mode_result = esp_lcd_panel_io_rx_param(
            touch_handle->io, kFt3168PowerModeRegister, &mode, 1);
        ESP_LOGI(kTag,
                 "TOUCH_SAMPLE seq=%lu failures=%lu stage=%s result=%s duration_us=%lld contacts=%u panel_sleep_requested=%u irq=%d mode_io=%s mode=%d",
                 static_cast<unsigned long>(attempts), static_cast<unsigned long>(failures),
                 read_result == ESP_OK ? "extract" : "read", esp_err_to_name(result),
                 static_cast<long long>(completed_us - started_us),
                 result == ESP_OK ? touch_count : 0, instance.display_sleep_requested(),
                 gpio_get_level(BSP_LCD_TOUCH_INT), esp_err_to_name(mode_result),
                 mode_result == ESP_OK ? static_cast<int>(mode) : -1);
        if (mode_result != ESP_OK) {
            const auto probe_result = i2c_master_probe(bsp_i2c_get_handle(),
                ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, kTouchTransactionTimeoutMs);
            ESP_LOGI(kTag, "TOUCH_ADDRESS_PROBE result=%s", esp_err_to_name(probe_result));
            request_touch_bus_quiet_diagnostic(probe_result);
        }
    }
    // Publish only after the genuine sample and all its diagnostics finish.
    // The ordinary input callback retains sole ownership of health/gestures.
    const auto evidence_completed_us = esp_timer_get_time();
    portENTER_CRITICAL(&touch_diagnostic_mux);
    ++last_touch_read_evidence.sequence;
    last_touch_read_evidence.result = result;
    last_touch_read_evidence.completed_us = evidence_completed_us;
    portEXIT_CRITICAL(&touch_diagnostic_mux);
    return result;
}

void record_touch_read_success() {
    ++successful_touch_reads;
    consecutive_touch_errors = 0;
    const auto now = esp_timer_get_time();
    if (touch_irq_registered) {
        nightglass::core::health_registry().set(
            "touch", nightglass::core::HealthState::ok,
            "Controller responding; LVGL input and GPIO38 IRQ registered");
        touch_degraded = false;
    } else {
        nightglass::core::health_registry().set(
            "touch", nightglass::core::HealthState::degraded,
            "Controller responding, but GPIO38 touch wake is unavailable");
        touch_degraded = true;
    }
    if (last_touch_liveness_log_us == 0 ||
        now - last_touch_liveness_log_us >= kTouchLivenessLogPeriodUs) {
        ESP_LOGI(kTag,
                 "TOUCH_LIVENESS controller=1 input=%u irq_registered=%u reads=%lu",
                 touch_input != nullptr, touch_irq_registered,
                 static_cast<unsigned long>(successful_touch_reads));
        ESP_LOGI(kTag, "RUNTIME_MEMORY internal_free=%lu largest=%lu",
                 static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        last_touch_liveness_log_us = now;
    }
}

void handle_touch_error(lv_indev_t *input, lv_indev_data_t *data,
                        const char *operation, esp_err_t error) {
    if (consecutive_touch_errors < 255) ++consecutive_touch_errors;
    if (consecutive_touch_errors <= 2) {
        // Preserve an in-progress press across a bounded missed sample. An
        // ordinary RELEASED here would synthesize CLICKED on the active
        // control even though the finger never left the panel.
        data->state = last_touch_state;
        data->point = last_touch_point;
    } else {
        // Cancel the gesture without delivering RELEASED/CLICKED. LVGL's
        // reset query terminates processing of this sample and clears the
        // active object before the released state can be interpreted.
        lv_indev_reset(input, nullptr);
        data->state = LV_INDEV_STATE_RELEASED;
        last_touch_state = LV_INDEV_STATE_RELEASED;
        if (!touch_degraded) {
            nightglass::core::health_registry().set(
                "touch", nightglass::core::HealthState::degraded,
                "Touch samples failing; active gesture cancelled safely");
            touch_degraded = true;
        }
    }

    const auto now = esp_timer_get_time();
    if (last_touch_error_log_us == 0 || now - last_touch_error_log_us >= 10'000'000) {
        ESP_LOGW(kTag, "%s skipped: %s", operation, esp_err_to_name(error));
        last_touch_error_log_us = now;
    }
}

void touchpad_read(lv_indev_t *input, lv_indev_data_t *data) {
    data->state = LV_INDEV_STATE_RELEASED;
    if (!touch_handle) return;

    std::uint8_t touch_count = 0;
    esp_lcd_touch_point_data_t point{};
    const auto sample_result = read_touch_controller(point, touch_count);
    if (sample_result != ESP_OK) {
        handle_touch_error(input, data, "Touch sample", sample_result);
        return;
    }

    record_touch_read_success();
    if (touch_count > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    if (data->state == LV_INDEV_STATE_PRESSED &&
        last_touch_state != LV_INDEV_STATE_PRESSED) {
        ESP_LOGI(kTag, "TOUCH_CONTACT x=%ld y=%ld",
                 static_cast<long>(data->point.x), static_cast<long>(data->point.y));
    }
    last_touch_state = data->state;
    last_touch_point = data->point;
}

void round_display_area(lv_area_t *area) {
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
    if (area->x2 >= BSP_LCD_H_RES) area->x2 = BSP_LCD_H_RES - 1;
    if (area->y2 >= BSP_LCD_V_RES) area->y2 = BSP_LCD_V_RES - 1;
}

bool configure_safe_output(gpio_num_t gpio, bool configure_sleep_pull_down) {
    const gpio_config_t config{
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK || gpio_set_level(gpio, 0) != ESP_OK ||
        gpio_get_level(gpio) != 0) {
        return false;
    }
    if (!configure_sleep_pull_down) return true;

    // GPIO46 drives the active-high audio PA. During light sleep, release the
    // active output and retain a pull-down rather than trusting the last
    // output latch. This is a separate physical barrier from codec mute.
    return gpio_sleep_set_direction(gpio, GPIO_MODE_DISABLE) == ESP_OK &&
           gpio_sleep_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY) == ESP_OK &&
           gpio_sleep_sel_en(gpio) == ESP_OK;
}
}

bool take_touch_bus_quiet_request() {
    portENTER_CRITICAL(&touch_diagnostic_mux);
    const bool requested = touch_bus_quiet_requested;
    touch_bus_quiet_requested = false;
    portEXIT_CRITICAL(&touch_diagnostic_mux);
    return requested;
}

TouchReadEvidence touch_read_evidence() {
    portENTER_CRITICAL(&touch_diagnostic_mux);
    const auto evidence = last_touch_read_evidence;
    portEXIT_CRITICAL(&touch_diagnostic_mux);
    return evidence;
}

void request_touch_read() {
    if (touch_input) lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, touch_input);
}

esp_err_t activate_touch_from_monitor() {
    if (!instance.lock_display(20)) return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (touch_handle != nullptr && touch_handle->io != nullptr) {
        std::uint8_t mode = 0xff;
        result = esp_lcd_panel_io_rx_param(
            touch_handle->io, kFt3168PowerModeRegister, &mode, 1);
        if (result == ESP_OK && mode == 1) {
            const std::uint8_t active_mode = kFt3168ActiveMode;
            result = esp_lcd_panel_io_tx_param(
                touch_handle->io, kFt3168PowerModeRegister, &active_mode, 1);
        } else if (result == ESP_OK && mode != kFt3168ActiveMode) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    instance.unlock_display();
    return result;
}

std::uint32_t touch_irq_sequence() {
    portENTER_CRITICAL(&touch_diagnostic_mux);
    const auto sequence = touch_diagnostic_irq_sequence;
    portEXIT_CRITICAL(&touch_diagnostic_mux);
    return sequence;
}

TouchBusQuietSample sample_touch_bus_quiet_diagnostic() {
    TouchBusQuietSample sample;
    sample.locked = instance.lock_display(20);
    if (sample.locked && touch_handle != nullptr && touch_handle->io != nullptr) {
        // Raw, documented register reads do not alter the driver's coordinate
        // buffers or LVGL state and never count as a normal touch sample.
        std::uint8_t value = 0xff;
        sample.count_result = esp_lcd_panel_io_rx_param(touch_handle->io, 0x02, &value, 1);
        if (sample.count_result == ESP_OK) sample.point_count = value;
        value = 0xff;
        sample.mode_result = esp_lcd_panel_io_rx_param(
            touch_handle->io, kFt3168PowerModeRegister, &value, 1);
        if (sample.mode_result == ESP_OK) sample.power_mode = value;
        if (sample.count_result != ESP_OK || sample.mode_result != ESP_OK) {
            const auto bus = bsp_i2c_get_handle();
            if (bus != nullptr) {
                sample.probe_result = i2c_master_probe(
                    bus, ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, kTouchTransactionTimeoutMs);
            }
        }
    }
    // GPIO9 is output-only; gpio_get_level() cannot report its physical level
    // with the input path disabled. Preserve unknown, not a false held reset.
    sample.reset_level = -1;
    sample.irq_level = gpio_get_level(BSP_LCD_TOUCH_INT);
    sample.irq_sequence = touch_irq_sequence();
    sample.sampled_at_us = esp_timer_get_time();
    if (sample.locked) instance.unlock_display();
    return sample;
}

nightglass::core::Status Board::prepare_safe_outputs() {
    // Fail-safe the transistor-driven vibration output before any peripheral
    // or UI startup can delay service initialization.
    if (!configure_safe_output(GPIO_NUM_18, false)) {
        return {nightglass::core::StatusCode::io_error, "haptic fail-safe failed"};
    }
    // GPIO46 is the ES8311 power-amplifier enable. Keep it physically low
    // before the display, I2C, or any codec code starts. The audio service
    // alone may raise it during a bounded explicit speaker diagnostic.
    if (!configure_safe_output(GPIO_NUM_46, true)) {
        return {nightglass::core::StatusCode::io_error, "audio PA fail-safe failed"};
    }
    return nightglass::core::Status::Ok();
}

nightglass::core::Status Board::start_essential() {
    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_config.task_stack = kLvglTaskStackBytes;
    // Keep the measured stack headroom without consuming another 16 KiB of
    // scarce radio/DMA RAM. PSRAM XIP keeps the cache available during NVS
    // writes from UI callbacks; the release verifier requires both XIP flags.
    lvgl_config.task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (lvgl_port_init(&lvgl_config) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "LVGL port initialization failed"};
    }

    // The vendor 1.0.7 convenience path incorrectly registers this QSPI
    // SH8601 panel as an RGB panel. That writes an RGB callback table through
    // an incompatible panel handle and corrupts TLSF metadata before BLE or
    // audio starts. Build the public BSP primitives through the SPI/I80 LVGL
    // path instead, and keep transfers bounded to the eight-row draw buffer.
    const bsp_display_config_t panel_config{
        .max_transfer_sz = BSP_LCD_H_RES * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT *
                           BSP_LCD_BITS_PER_PIXEL / 8,
    };
    if (bsp_display_new(&panel_config, &panel_handle, &panel_io) != ESP_OK) {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::failed,
                                           "BSP display initialization failed");
        return {nightglass::core::StatusCode::io_error, "display initialization failed"};
    }

    const lvgl_port_display_cfg_t display_config{
        .io_handle = panel_io,
        .panel_handle = panel_handle,
        .control_handle = nullptr,
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT,
        .double_buffer = false,
        .trans_size = 0,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .rounder_cb = round_display_area,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    display_handle = lvgl_port_add_disp(&display_config);
    if (display_handle == nullptr || create_touch_controller() != ESP_OK) {
        nightglass::core::health_registry().set("touch", nightglass::core::HealthState::failed,
                                           "BSP touch initialization failed");
        return {nightglass::core::StatusCode::io_error, "touch initialization failed"};
    }
    TouchConfigurationIo touch_configuration;
    std::uint8_t touch_device_id = 0xff;
    std::uint8_t touch_power_mode = 0xff;
    if (!configure_ft3168(touch_configuration, touch_device_id, touch_power_mode)) {
        ESP_LOGE(kTag, "FT3168 setup failed: id=0x%02x mode=0x%02x io=%s",
                 touch_device_id, touch_power_mode,
                 esp_err_to_name(touch_configuration.error));
        nightglass::core::health_registry().set(
            "touch", nightglass::core::HealthState::failed,
            "FT3168 identity or active-mode verification failed");
        return {nightglass::core::StatusCode::io_error, "FT3168 configuration failed"};
    }
    ESP_LOGI(kTag, "TOUCH_CONFIG chip=FT3168 id=0x%02x mode=0x%02x factory_tuning=1",
             touch_device_id, touch_power_mode);
    ESP_LOGI(kTag, "TOUCH_WIRING reset=%d irq=%d lcd_reset=%d transaction_timeout_ms=%d probe_period_ms=%llu",
             BSP_LCD_TOUCH_RST, BSP_LCD_TOUCH_INT, BSP_LCD_RST,
             kTouchTransactionTimeoutMs,
             static_cast<unsigned long long>(kTouchProbePeriodUs / 1000));
    // esp_lvgl_port 2.7.2 wraps transient touch-controller read failures in
    // ESP_ERROR_CHECK(), turning an ordinary missed sample into a full-system
    // abort from the LVGL task. Nightglass owns the input callback instead:
    // failed samples are released and rate-limited in the log, while the
    // display, clock, BLE, and audio services continue running.
    if (!lvgl_port_lock(0)) {
        return {nightglass::core::StatusCode::timeout, "LVGL touch lock failed"};
    }
    touch_input = lv_indev_create();
    if (touch_input) {
        lv_indev_set_type(touch_input, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(touch_input, touchpad_read);
        lv_indev_set_disp(touch_input, display_handle);
        touch_irq_registered = touch_handle->config.int_gpio_num != GPIO_NUM_NC &&
            esp_lcd_touch_register_interrupt_callback_with_data(
                touch_handle, touch_interrupt, nullptr) == ESP_OK;
        if (touch_irq_registered) {
            lv_indev_set_mode(touch_input, LV_INDEV_MODE_EVENT);
        }
    }

    std::uint8_t initial_touch_count = 0;
    esp_lcd_touch_point_data_t initial_touch_point{};
    const auto initial_touch_result = touch_input
        ? read_touch_controller(initial_touch_point, initial_touch_count)
        : ESP_ERR_INVALID_STATE;
    if (initial_touch_result == ESP_OK) {
        record_touch_read_success();
    } else {
        touch_degraded = true;
        nightglass::core::health_registry().set(
            "touch", nightglass::core::HealthState::degraded,
            "Touch controller did not answer initial liveness read");
        ESP_LOGW(kTag, "Initial touch liveness read failed: %s",
                 esp_err_to_name(initial_touch_result));
    }
    lvgl_port_unlock();
    if (touch_input == nullptr) {
        nightglass::core::health_registry().set("touch", nightglass::core::HealthState::failed,
                                           "BSP touch initialization failed");
        return {nightglass::core::StatusCode::io_error, "touch initialization failed"};
    }

    if (bsp_display_brightness_init() != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "brightness initialization failed"};
    }

    // The vendor BSP powers the AMOLED up at 100%. Use a conservative bench
    // level until PowerService owns adaptive brightness and burn-in policy.
    const bool brightness_ready = set_brightness(30).is_ok();
    if (!brightness_ready) {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::degraded,
                                           "Display online; brightness control failed");
    } else {
        nightglass::core::health_registry().set("display", nightglass::core::HealthState::ok,
                                           "BSP display and touch started");
    }

    const esp_timer_create_args_t touch_probe_config{
        .callback = touch_probe_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "touch_liveness",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&touch_probe_config, &touch_probe_timer) != ESP_OK ||
        esp_timer_start_periodic(touch_probe_timer, kTouchProbePeriodUs) != ESP_OK) {
        nightglass::core::health_registry().set(
            "touch", nightglass::core::HealthState::degraded,
            "Touch liveness probe timer unavailable");
        touch_degraded = true;
    }
    return nightglass::core::Status::Ok();
}

i2c_master_bus_handle_t Board::i2c_bus() const { return bsp_i2c_get_handle(); }

nightglass::core::Status Board::sleep_display() {
    if (display_sleeping_) return nightglass::core::Status::Ok();
    if (!panel_io) {
        return {nightglass::core::StatusCode::invalid_state, "display panel unavailable"};
    }

    if (esp_lcd_panel_disp_on_off(panel_handle, false) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "display-off command failed"};
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (esp_lcd_panel_io_tx_param(panel_io, sh8601_qspi_command(0x10), nullptr, 0) != ESP_OK) {
        // DISPOFF succeeded but SLPIN did not. Restore scan-out so the cached
        // software state continues to describe the physical panel.
        esp_lcd_panel_disp_on_off(panel_handle, true);
        return {nightglass::core::StatusCode::io_error, "display sleep-in command failed"};
    }
    vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(5)));
    display_sleeping_ = true;
    ESP_LOGI(kTag, "DISPLAY_SLEEP command_path=sh8601_qspi cached_sleep=1");
    return nightglass::core::Status::Ok();
}

nightglass::core::Status Board::wake_display() {
    if (!display_sleeping_) return nightglass::core::Status::Ok();
    if (!panel_io) {
        return {nightglass::core::StatusCode::invalid_state, "display panel unavailable"};
    }

    if (esp_lcd_panel_io_tx_param(panel_io, sh8601_qspi_command(0x11), nullptr, 0) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "display sleep-out command failed"};
    }
    // SH8601 requires the full sleep-out recovery interval before DISPON.
    vTaskDelay(pdMS_TO_TICKS(120));
    if (esp_lcd_panel_disp_on_off(panel_handle, true) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "display-on command failed"};
    }
    display_sleeping_ = false;
    ESP_LOGI(kTag, "DISPLAY_WAKE command_path=sh8601_qspi cached_sleep=0");
    if (bsp_display_brightness_set(target_brightness_) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "brightness restore failed"};
    }
    return nightglass::core::Status::Ok();
}

nightglass::core::Status Board::set_brightness(std::uint8_t percent) {
    if (percent > 100) {
        return {nightglass::core::StatusCode::invalid_state, "brightness outside 0-100"};
    }
    if (percent == 0) return sleep_display();

    target_brightness_ = percent;
    if (display_sleeping_) return wake_display();

    // The vendor BSP currently returns ESP_OK even if its DCS write fails.
    // Treat this as a requested/cached level until physical HIL confirms it.
    if (bsp_display_brightness_set(percent) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error, "brightness request failed"};
    }
    return nightglass::core::Status::Ok();
}

std::uint8_t Board::brightness() const {
    if (display_sleeping_) return 0;
    const int value = bsp_display_brightness_get();
    return value < 0 ? 0 : value > 100 ? 100 : static_cast<std::uint8_t>(value);
}

bool Board::lock_display(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void Board::unlock_display() { bsp_display_unlock(); }
Board &board() { return instance; }

}  // namespace nightglass::bsp
