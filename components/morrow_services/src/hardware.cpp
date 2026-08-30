#include "morrow/services/hardware.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "morrow/core/health.hpp"

namespace morrow::services {
namespace {

constexpr char kTag[] = "morrow_hw";
constexpr std::uint8_t kRtcAddress = 0x51;
constexpr std::uint8_t kPmicAddress = 0x34;
constexpr std::uint8_t kPmicChipId = 0x4A;
constexpr std::uint8_t kPmicLdoOnOff0 = 0x90;
constexpr std::uint8_t kPmicAldo3Voltage = 0x94;
constexpr std::uint8_t kPmicAldo3EnableBit = 1U << 2U;
// This physical unit completed GPIO18/ALDO3 HIL with no mechanical response.
// Waveshare exposes P1/P2 motor pads but does not list an installed actuator.
constexpr bool kHapticActuatorPresent = false;
constexpr std::uint16_t kHapticSupplyMv = 3000;
constexpr std::uint8_t kHapticSupplyCode =
    static_cast<std::uint8_t>((kHapticSupplyMv - 500U) / 100U);
constexpr std::uint8_t kImuAddress = 0x6B;
constexpr std::uint8_t kImuChipId = 0x05;
constexpr gpio_num_t kHapticGpio = GPIO_NUM_18;
constexpr std::int64_t kHapticDebounceUs = 1'000'000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(40);
constexpr int kI2cTimeoutMs = 20;

struct HapticCommand {
    std::uint16_t duration_ms;
};

HardwareService instance;
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
HardwareSnapshot current{};
QueueHandle_t haptic_queue = nullptr;
TaskHandle_t service_task = nullptr;
i2c_master_dev_handle_t rtc_device = nullptr;
i2c_master_dev_handle_t pmic_device = nullptr;
i2c_master_dev_handle_t imu_device = nullptr;
bool imu_ready = false;
bool haptic_output_ready = false;
bool haptic_supply_cleanup_verified = true;
esp_timer_handle_t haptic_timer = nullptr;

std::uint8_t from_bcd(std::uint8_t value) {
    return static_cast<std::uint8_t>(((value >> 4U) * 10U) + (value & 0x0FU));
}

esp_err_t read_register(i2c_master_dev_handle_t device, std::uint8_t reg,
                        std::uint8_t *data, std::size_t length) {
    return i2c_master_transmit_receive(device, &reg, 1, data, length, kI2cTimeoutMs);
}

esp_err_t write_register(i2c_master_dev_handle_t device, std::uint8_t reg,
                         std::uint8_t value) {
    const std::uint8_t payload[] = {reg, value};
    return i2c_master_transmit(device, payload, sizeof(payload), kI2cTimeoutMs);
}

bool add_i2c_device(i2c_master_bus_handle_t bus, std::uint16_t address,
                    i2c_master_dev_handle_t *device) {
    const i2c_device_config_t config{
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = false},
    };
    return i2c_master_bus_add_device(bus, &config, device) == ESP_OK;
}

void publish_rtc() {
    RtcSnapshot next{};
    next.sampled_at_us = esp_timer_get_time();
    std::uint8_t raw[7]{};
    if (rtc_device && read_register(rtc_device, 0x04, raw, sizeof(raw)) == ESP_OK) {
        next.present = true;
        next.second = from_bcd(raw[0] & 0x7FU);
        next.minute = from_bcd(raw[1] & 0x7FU);
        next.hour = from_bcd(raw[2] & 0x3FU);
        next.day = from_bcd(raw[3] & 0x3FU);
        next.weekday = raw[4] & 0x07U;
        next.month = from_bcd(raw[5] & 0x1FU);
        next.year = static_cast<std::uint16_t>(2000U + from_bcd(raw[6]));
        next.valid = (raw[0] & 0x80U) == 0 && next.second < 60 && next.minute < 60 &&
                     next.hour < 24 && next.day >= 1 && next.day <= 31 &&
                     next.weekday <= 6 && next.month >= 1 && next.month <= 12;
    }

    portENTER_CRITICAL(&snapshot_mux);
    current.rtc = next;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void publish_battery() {
    BatterySnapshot next{};
    next.sampled_at_us = esp_timer_get_time();
    std::uint8_t status[2]{};
    if (pmic_device && read_register(pmic_device, 0x00, status, sizeof(status)) == ESP_OK) {
        next.pmic_present = true;
        next.battery_present = (status[0] & (1U << 3U)) != 0;
        const auto power_state = static_cast<std::uint8_t>(status[1] >> 5U);
        next.charging = power_state == 1;
        next.discharging = power_state == 2;

        if (next.battery_present) {
            std::uint8_t voltage[2]{};
            if (read_register(pmic_device, 0x34, voltage, sizeof(voltage)) == ESP_OK) {
                next.voltage_mv = static_cast<std::uint16_t>(((voltage[0] & 0x1FU) << 8U) |
                                                             voltage[1]);
                next.voltage_valid = next.voltage_mv >= 2500 && next.voltage_mv <= 5000;
            }
            std::uint8_t percent = 0;
            if (read_register(pmic_device, 0xA4, &percent, 1) == ESP_OK && percent <= 100) {
                next.percent = percent;
                next.percent_valid = true;
            }
        }
    }

    portENTER_CRITICAL(&snapshot_mux);
    current.battery = next;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void publish_motion() {
    MotionSnapshot next{};
    next.present = imu_ready;
    next.sampled_at_us = esp_timer_get_time();
    if (imu_ready) {
        std::uint8_t data[12]{};
        if (read_register(imu_device, 0x35, data, sizeof(data)) == ESP_OK) {
            const auto raw_ax = static_cast<std::int16_t>((data[1] << 8U) | data[0]);
            const auto raw_ay = static_cast<std::int16_t>((data[3] << 8U) | data[2]);
            const auto raw_az = static_cast<std::int16_t>((data[5] << 8U) | data[4]);
            const auto raw_gx = static_cast<std::int16_t>((data[7] << 8U) | data[6]);
            const auto raw_gy = static_cast<std::int16_t>((data[9] << 8U) | data[8]);
            const auto raw_gz = static_cast<std::int16_t>((data[11] << 8U) | data[10]);
            next.valid = true;
            next.accel_x_g = static_cast<float>(raw_ax) / 4096.0F;
            next.accel_y_g = static_cast<float>(raw_ay) / 4096.0F;
            next.accel_z_g = static_cast<float>(raw_az) / 4096.0F;
            next.gyro_x_dps = static_cast<float>(raw_gx) / 64.0F;
            next.gyro_y_dps = static_cast<float>(raw_gy) / 64.0F;
            next.gyro_z_dps = static_cast<float>(raw_gz) / 64.0F;

            const float accel_energy = next.accel_x_g * next.accel_x_g +
                                       next.accel_y_g * next.accel_y_g +
                                       next.accel_z_g * next.accel_z_g;
            const float gyro_energy = next.gyro_x_dps * next.gyro_x_dps +
                                      next.gyro_y_dps * next.gyro_y_dps +
                                      next.gyro_z_dps * next.gyro_z_dps;
            const bool candidate = accel_energy < 0.7744F || accel_energy > 1.2544F ||
                                   gyro_energy > 64.0F;
            static bool stable_moving = false;
            static bool pending_moving = false;
            static std::int64_t pending_since_us = 0;
            if (candidate != pending_moving) {
                pending_moving = candidate;
                pending_since_us = next.sampled_at_us;
            } else if (candidate != stable_moving &&
                       next.sampled_at_us - pending_since_us >= 300'000) {
                stable_moving = candidate;
            }
            next.moving = stable_moving;
        }
    }

    portENTER_CRITICAL(&snapshot_mux);
    current.motion = next;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void set_haptic_state(bool active, std::int64_t pulse_time, bool accepted) {
    portENTER_CRITICAL(&snapshot_mux);
    current.haptic.pulse_active = active;
    if (accepted) {
        current.haptic.last_pulse_us = pulse_time;
        ++current.haptic.accepted_pulses;
    }
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void publish_haptic_failure() {
    portENTER_CRITICAL(&snapshot_mux);
    current.haptic.pulse_active = false;
    ++current.haptic.failed_pulses;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
}

void haptic_timer_callback(void *) {
    if (gpio_set_level(kHapticGpio, 0) == ESP_OK) {
        set_haptic_state(false, 0, false);
        ESP_LOGI(kTag, "Haptic cutoff: GPIO18 low");
    } else {
        publish_haptic_failure();
        ESP_LOGE(kTag, "Haptic cutoff failed: GPIO18 state unknown");
    }
}

void publish_haptic_supply(bool state_known, bool enabled, std::uint16_t voltage_mv) {
    portENTER_CRITICAL(&snapshot_mux);
    current.haptic.supply_state_known = state_known;
    current.haptic.supply_enabled = enabled;
    current.haptic.supply_voltage_mv = voltage_mv;
    portEXIT_CRITICAL(&snapshot_mux);
}

bool disable_haptic_supply(std::uint8_t cached_enable, bool cached_valid) {
    if (gpio_set_level(kHapticGpio, 0) != ESP_OK) {
        ESP_LOGE(kTag, "GPIO18 low could not be confirmed during ALDO3 shutdown");
    }
    if (!pmic_device) {
        publish_haptic_supply(false, false, 0);
        ESP_LOGE(kTag, "Haptic ALDO3 shutdown unavailable: PMIC handle missing");
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::uint8_t enable = cached_enable;
        if (!cached_valid &&
            read_register(pmic_device, kPmicLdoOnOff0, &enable, 1) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const auto disabled = static_cast<std::uint8_t>(enable & ~kPmicAldo3EnableBit);
        std::uint8_t verified_enable = 0;
        if (write_register(pmic_device, kPmicLdoOnOff0, disabled) == ESP_OK &&
            read_register(pmic_device, kPmicLdoOnOff0, &verified_enable, 1) == ESP_OK &&
            (verified_enable & kPmicAldo3EnableBit) == 0) {
            publish_haptic_supply(true, false, 0);
            return true;
        }

        cached_valid = false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    publish_haptic_supply(false, false, 0);
    ESP_LOGE(kTag, "Haptic ALDO3 shutdown could not be verified");
    return false;
}

bool configure_haptic_supply() {
    haptic_supply_cleanup_verified = true;
    if (!pmic_device) {
        haptic_supply_cleanup_verified = false;
        publish_haptic_supply(false, false, 0);
        return false;
    }
    if (gpio_set_level(kHapticGpio, 0) != ESP_OK) {
        haptic_supply_cleanup_verified = disable_haptic_supply(0, false);
        return false;
    }

    std::uint8_t enable = 0;
    std::uint8_t voltage = 0;
    if (read_register(pmic_device, kPmicLdoOnOff0, &enable, 1) != ESP_OK) {
        haptic_supply_cleanup_verified = disable_haptic_supply(0, false);
        return false;
    }
    if (read_register(pmic_device, kPmicAldo3Voltage, &voltage, 1) != ESP_OK) {
        haptic_supply_cleanup_verified = disable_haptic_supply(enable, true);
        return false;
    }

    const bool initially_enabled = (enable & kPmicAldo3EnableBit) != 0;
    const auto initial_mv = static_cast<std::uint16_t>((voltage & 0x1FU) * 100U + 500U);
    ESP_LOGI(kTag, "Haptic supply before ownership: ALDO3=%s %u mV",
             initially_enabled ? "on" : "off", initial_mv);

    const std::uint8_t requested_voltage =
        static_cast<std::uint8_t>((voltage & 0xE0U) | kHapticSupplyCode);
    const bool voltage_written =
        write_register(pmic_device, kPmicAldo3Voltage, requested_voltage) == ESP_OK;
    const bool enable_written =
        voltage_written &&
        write_register(pmic_device, kPmicLdoOnOff0,
                       static_cast<std::uint8_t>(enable | kPmicAldo3EnableBit)) == ESP_OK;

    std::uint8_t verified_enable = 0;
    std::uint8_t verified_voltage = 0;
    const bool enable_read =
        enable_written &&
        read_register(pmic_device, kPmicLdoOnOff0, &verified_enable, 1) == ESP_OK;
    const bool voltage_read =
        enable_read &&
        read_register(pmic_device, kPmicAldo3Voltage, &verified_voltage, 1) == ESP_OK;
    const bool verified =
        voltage_read &&
        (verified_enable & kPmicAldo3EnableBit) != 0 &&
        (verified_voltage & 0x1FU) == kHapticSupplyCode;

    if (!verified) {
        haptic_supply_cleanup_verified =
            disable_haptic_supply(verified_enable, enable_read);
    }

    if (verified) publish_haptic_supply(true, true, kHapticSupplyMv);
    ESP_LOGI(kTag, "Haptic supply ownership: ALDO3=%s %u mV",
             verified ? "on" : "failed", verified ? kHapticSupplyMv : 0);
    return verified;
}

void probe_devices(i2c_master_bus_handle_t bus_handle) {
    const bool rtc_added = add_i2c_device(bus_handle, kRtcAddress, &rtc_device);
    morrow::core::health_registry().set("rtc", rtc_added ? morrow::core::HealthState::degraded
                                                         : morrow::core::HealthState::failed,
                                       rtc_added ? "RTC registered; first read pending"
                                                 : "RTC I2C registration failed");

    const bool pmic_added = add_i2c_device(bus_handle, kPmicAddress, &pmic_device);
    bool pmic_identified = false;
    if (pmic_added) {
        std::uint8_t chip_id = 0;
        pmic_identified = read_register(pmic_device, 0x03, &chip_id, 1) == ESP_OK &&
                          chip_id == kPmicChipId;
        if (pmic_identified) {
            std::uint8_t adc_control = 0;
            if (read_register(pmic_device, 0x30, &adc_control, 1) == ESP_OK) {
                // Read-modify-write only the documented battery-voltage ADC enable bit.
                write_register(pmic_device, 0x30, adc_control | 0x01U);
            }
        }
    }
    if (!pmic_identified) pmic_device = nullptr;
    morrow::core::health_registry().set("pmic", pmic_identified
                                                        ? morrow::core::HealthState::degraded
                                                        : morrow::core::HealthState::failed,
                                       pmic_identified ? "AXP2101 identified; first read pending"
                                                       : "AXP2101 identification failed");

    bool haptic_supply_ready = false;
    if (pmic_identified) {
        if constexpr (kHapticActuatorPresent) {
            haptic_supply_ready = configure_haptic_supply();
        } else {
            haptic_supply_cleanup_verified = disable_haptic_supply(0, false);
        }
    } else {
        haptic_supply_cleanup_verified = false;
        publish_haptic_supply(false, false, 0);
    }
    const bool haptic_ready = kHapticActuatorPresent && haptic_output_ready &&
                              haptic_supply_ready;
    portENTER_CRITICAL(&snapshot_mux);
    current.haptic.actuator_present = kHapticActuatorPresent;
    current.haptic.ready = haptic_ready;
    ++current.sequence;
    portEXIT_CRITICAL(&snapshot_mux);
    morrow::core::health_registry().set(
        "haptic", haptic_ready ? morrow::core::HealthState::ok
        : !kHapticActuatorPresent && haptic_supply_cleanup_verified
            ? morrow::core::HealthState::ok
            : morrow::core::HealthState::failed,
        haptic_ready ? "GPIO18 and ALDO3 ready; actuator verified"
        : !kHapticActuatorPresent && haptic_supply_cleanup_verified
            ? "Actuator not fitted; ALDO3 disabled"
        : !haptic_supply_cleanup_verified ? "Haptic disabled; ALDO3 state unverified"
                                          : "Haptic GPIO or ALDO3 initialization failed");

    bool imu_added = add_i2c_device(bus_handle, kImuAddress, &imu_device);
    std::uint8_t imu_id = 0;
    if (imu_added && write_register(imu_device, 0x60, 0xB0) == ESP_OK) {
        // Vendor maximum reset time is 15 ms. Reset removes any dependency on
        // register state left behind by the previous firmware.
        vTaskDelay(pdMS_TO_TICKS(20));
    } else {
        imu_added = false;
    }
    imu_ready = imu_added && read_register(imu_device, 0x00, &imu_id, 1) == ESP_OK &&
                imu_id == kImuChipId;
    if (imu_ready) {
        // CTRL1: little-endian with address auto-increment. CTRL2: 8 g at
        // 31.25 Hz. CTRL3: 512 dps at 28.025 Hz.
        // CTRL7 enables accelerometer and gyro for this explicit diagnostics screen.
        imu_ready = write_register(imu_device, 0x02, 0x40) == ESP_OK &&
                    write_register(imu_device, 0x03, 0x28) == ESP_OK &&
                    write_register(imu_device, 0x04, 0x58) == ESP_OK &&
                    write_register(imu_device, 0x08, 0x03) == ESP_OK;
    }
    morrow::core::health_registry().set("imu", imu_ready ? morrow::core::HealthState::degraded
                                                         : morrow::core::HealthState::failed,
                                       imu_ready ? "QMI8658 identified; first read pending"
                                                 : "QMI8658 initialization failed");

    ESP_LOGI(kTag, "Hardware probes: rtc=%d pmic=%d imu=%d", rtc_added,
             pmic_identified, imu_ready);
}

void hardware_task(void *context) {
    probe_devices(static_cast<i2c_master_bus_handle_t>(context));
    std::int64_t last_rtc_us = -1'000'000;
    std::int64_t last_battery_us = -2'000'000;

    while (true) {
        const std::int64_t now = esp_timer_get_time();
        HapticCommand command{};
        if (haptic_queue && xQueueReceive(haptic_queue, &command, 0) == pdTRUE) {
            esp_timer_stop(haptic_timer);
            if (esp_timer_start_once(haptic_timer,
                                     static_cast<std::uint64_t>(command.duration_ms) * 1000U) == ESP_OK &&
                gpio_set_level(kHapticGpio, 1) == ESP_OK) {
                set_haptic_state(true, now, true);
                ESP_LOGI(kTag, "Haptic active: GPIO18 high for %u ms, ALDO3=%u mV",
                         command.duration_ms, kHapticSupplyMv);
            } else {
                esp_timer_stop(haptic_timer);
                gpio_set_level(kHapticGpio, 0);
                publish_haptic_failure();
            }
        }

        publish_motion();
        if (now - last_rtc_us >= 1'000'000) {
            publish_rtc();
            last_rtc_us = now;
        }
        if (now - last_battery_us >= 2'000'000) {
            publish_battery();
            last_battery_us = now;
        }
        vTaskDelay(kPollTicks);
    }
}

}  // namespace

morrow::core::Status HardwareService::start(i2c_master_bus_handle_t bus_handle) {
    if (!bus_handle) return {morrow::core::StatusCode::invalid_state, "I2C bus unavailable"};
    if (service_task) return morrow::core::Status::Ok();

    const gpio_config_t haptic_config{
        .pin_bit_mask = 1ULL << kHapticGpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const bool haptic_gpio_ready = gpio_config(&haptic_config) == ESP_OK &&
                                   gpio_set_level(kHapticGpio, 0) == ESP_OK;
    haptic_queue = xQueueCreate(1, sizeof(HapticCommand));
    const esp_timer_create_args_t timer_config{
        .callback = haptic_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haptic_cutoff",
        .skip_unhandled_events = true,
    };
    const bool timer_ready = esp_timer_create(&timer_config, &haptic_timer) == ESP_OK;
    portENTER_CRITICAL(&snapshot_mux);
    haptic_output_ready = haptic_gpio_ready && haptic_queue && timer_ready;
    current.haptic.ready = false;
    portEXIT_CRITICAL(&snapshot_mux);
    morrow::core::health_registry().set(
        "haptic", haptic_output_ready ? morrow::core::HealthState::degraded
                                       : morrow::core::HealthState::failed,
        haptic_output_ready ? "GPIO18 ready; ALDO3 probe pending"
                            : "Haptic output initialization failed");

    if (xTaskCreatePinnedToCore(hardware_task, "hardware_service", 6144, bus_handle,
                               4, &service_task, 0) != pdPASS) {
        service_task = nullptr;
        gpio_set_level(kHapticGpio, 0);
        return {morrow::core::StatusCode::no_memory, "hardware task creation failed"};
    }
    ESP_LOGI(kTag, "Hardware service task started: haptic_output=%d", haptic_output_ready);
    return morrow::core::Status::Ok();
}

HardwareSnapshot HardwareService::snapshot() const {
    HardwareSnapshot copy{};
    portENTER_CRITICAL(&snapshot_mux);
    copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

bool HardwareService::request_haptic(std::uint16_t duration_ms) {
    if (!haptic_queue || duration_ms < 20 || duration_ms > 250) return false;
    const auto state = snapshot().haptic;
    const auto now = esp_timer_get_time();
    if (!state.ready || state.pulse_active || now - state.last_pulse_us < kHapticDebounceUs) {
        return false;
    }
    const HapticCommand command{duration_ms};
    return xQueueSend(haptic_queue, &command, 0) == pdTRUE;
}

HardwareService &hardware_service() { return instance; }

}  // namespace morrow::services
