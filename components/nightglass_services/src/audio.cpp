#include "nightglass/services/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(CONFIG_NIGHTGLASS_AUDIO_ENABLED) && CONFIG_NIGHTGLASS_AUDIO_ENABLED
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_if.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/power.hpp"
#define NIGHTGLASS_AUDIO_RUNTIME 1
#else
#define NIGHTGLASS_AUDIO_RUNTIME 0
#endif

namespace nightglass::services {
namespace {

constexpr std::size_t kMaxIoBytes = 4096;

#if NIGHTGLASS_AUDIO_RUNTIME
constexpr char kTag[] = "nightglass_audio";
constexpr std::uint32_t kSampleRateHz = 16000;
constexpr std::uint8_t kChannels = 2;
constexpr std::size_t kFrameBytes = kChannels * sizeof(std::int16_t);
// A 32 ms diagnostic is long enough to measure/identify, but deliberately
// small enough that its temporary DMA allocation coexists with the AMOLED.
constexpr std::size_t kDiagnosticBytes = 2048;
constexpr std::size_t kDiagnosticFrames = kDiagnosticBytes / kFrameBytes;
constexpr std::uint32_t kIoTimeoutMs = 150;
constexpr std::uint32_t kAudioTaskStackBytes = 4096;
constexpr std::int16_t kToneAmplitude = 3500;
constexpr std::uint8_t kTestToneVolumePercent = 60;
// The esp_codec_dev wrapper's volume setter discards the codec driver's I2C
// result. Use the codec interface directly so PA enable is gated on a real
// gain-register write, not merely on the wrapper accepting the request.
constexpr float kTestToneGainDb = -20.0F;
constexpr gpio_num_t kAmplifierGpio = BSP_POWER_AMP_IO;

struct AudioResources {
    i2s_chan_handle_t channel{nullptr};
    const audio_codec_data_if_t *data_if{nullptr};
    const audio_codec_ctrl_if_t *ctrl_if{nullptr};
    const audio_codec_if_t *codec_if{nullptr};
    esp_codec_dev_handle_t device{nullptr};
    esp_codec_dev_type_t device_type{ESP_CODEC_DEV_TYPE_NONE};
    bool device_open{false};
};

struct CleanupResult {
    bool amplifier_disabled{false};
    bool resources_released{true};
};

struct OperationResult {
    nightglass::core::Status status{nightglass::core::Status::Ok()};
    std::size_t transferred_bytes{0};
    std::uint32_t capture_rms{0};
    bool amplifier_disabled{true};
    bool resources_released{true};
};

AudioService instance;
AudioSnapshot state{};
i2c_master_bus_handle_t audio_bus{nullptr};
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;

bool set_amplifier(bool enabled) {
    if (gpio_set_direction(kAmplifierGpio, GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
        gpio_set_level(kAmplifierGpio, enabled ? 1 : 0) != ESP_OK) {
        return false;
    }
    return gpio_get_level(kAmplifierGpio) == (enabled ? 1 : 0);
}

bool force_amplifier_off() { return set_amplifier(false); }

void mark_operation_active(AudioOperation operation, bool input, bool output,
                           bool amplifier_disabled) {
    (void)operation;
    portENTER_CRITICAL(&state_mux);
    state.hardware_initialized = true;
    state.hardware_active = true;
    state.input_ready = input;
    state.output_ready = output;
    state.output_volume = output ? kTestToneVolumePercent : 0;
    state.amplifier_disabled = amplifier_disabled;
    state.amplifier_disabled_verified = amplifier_disabled;
    ++state.sequence;
    portEXIT_CRITICAL(&state_mux);
}

bool create_i2s(AudioResources &resources, bool playback) {
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        static_cast<i2s_port_t>(CONFIG_BSP_I2S_NUM), I2S_ROLE_MASTER);
    // Two short DMA descriptors keep the one-shot diagnostic from competing
    // with the display's contiguous internal DMA requirement.
    channel_config.dma_desc_num = 2;
    channel_config.dma_frame_num = 128;
    channel_config.auto_clear = true;

    const esp_err_t channel_status = playback
                                         ? i2s_new_channel(&channel_config, &resources.channel, nullptr)
                                         : i2s_new_channel(&channel_config, nullptr, &resources.channel);
    if (channel_status != ESP_OK) return false;

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = playback ? BSP_I2S_DOUT : I2S_GPIO_UNUSED,
            .din = playback ? I2S_GPIO_UNUSED : BSP_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    if (i2s_channel_init_std_mode(resources.channel, &standard_config) != ESP_OK) return false;

    audio_codec_i2s_cfg_t data_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = playback ? nullptr : resources.channel,
        .tx_handle = playback ? resources.channel : nullptr,
        .clk_src = 0,
    };
    resources.data_if = audio_codec_new_i2s_data(&data_config);
    return resources.data_if != nullptr;
}

bool open_output(AudioResources &resources) {
    if (!create_i2s(resources, true)) return false;

    audio_codec_i2c_cfg_t i2c_config = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = audio_bus,
    };
    resources.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (resources.ctrl_if == nullptr) return false;

    // GPIO46 is intentionally *not* handed to the codec. ES8311 may unmute
    // itself while opening, but the Nightglass-owned physical PA gate remains
    // low until the bounded I2S write is about to begin.
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = resources.ctrl_if,
        .gpio_if = nullptr,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3, .pa_gain = 0.0},
        .no_dac_ref = false,
        .mclk_div = 256,
    };
    resources.codec_if = es8311_codec_new(&codec_config);
    if (resources.codec_if == nullptr) return false;

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = resources.codec_if,
        .data_if = resources.data_if,
    };
    resources.device_type = ESP_CODEC_DEV_TYPE_OUT;
    resources.device = esp_codec_dev_new(&device_config);
    if (resources.device == nullptr) return false;

    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = kChannels,
        .channel_mask = 0x03,
        .sample_rate = kSampleRateHz,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(resources.device, &sample) != ESP_CODEC_DEV_OK) return false;
    resources.device_open = true;
    // esp_codec_dev_set_out_vol() returns success even when the codec driver's
    // I2C write fails. Set the conservative test gain through the underlying
    // interface and require its return value before the physical PA gate can
    // be raised.
    if (resources.codec_if->set_vol == nullptr ||
        resources.codec_if->set_vol(resources.codec_if, kTestToneGainDb) != ESP_CODEC_DEV_OK) {
        return false;
    }
    return true;
}

bool open_input(AudioResources &resources) {
    if (!create_i2s(resources, false)) return false;

    audio_codec_i2c_cfg_t i2c_config = {
        .port = BSP_I2C_NUM,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = audio_bus,
    };
    resources.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (resources.ctrl_if == nullptr) return false;

    es7210_codec_cfg_t codec_config = {
        .ctrl_if = resources.ctrl_if,
        .master_mode = false,
        .mic_selected = static_cast<std::uint8_t>(ES7210_SEL_MIC1 | ES7210_SEL_MIC2),
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    resources.codec_if = es7210_codec_new(&codec_config);
    if (resources.codec_if == nullptr) return false;

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = resources.codec_if,
        .data_if = resources.data_if,
    };
    resources.device_type = ESP_CODEC_DEV_TYPE_IN;
    resources.device = esp_codec_dev_new(&device_config);
    if (resources.device == nullptr) return false;

    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = kChannels,
        .channel_mask = 0x03,
        .sample_rate = kSampleRateHz,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(resources.device, &sample) != ESP_CODEC_DEV_OK) return false;
    resources.device_open = true;
    return true;
}

CleanupResult release_resources(AudioResources &resources) {
    CleanupResult result{};
    // The PA is the actual safety boundary. Assert it before every best-effort
    // codec close, then assert/read it again after cleanup.
    result.amplifier_disabled = force_amplifier_off();

    // esp_codec_dev_close() intentionally discards the codec/data driver's
    // disable result. Invoke the codec interface directly first so an I2C
    // suspend failure cannot be reported as a clean diagnostic shutdown.
    if (resources.codec_if != nullptr) {
        bool codec_shutdown_ok = resources.codec_if->is_open != nullptr;
        if (codec_shutdown_ok && resources.codec_if->is_open(resources.codec_if)) {
            codec_shutdown_ok = resources.codec_if->enable != nullptr &&
                                resources.codec_if->enable(resources.codec_if, false) == ESP_CODEC_DEV_OK;
        }
        if (!codec_shutdown_ok) result.resources_released = false;
    }
    if (resources.data_if != nullptr && resources.device_open) {
        const bool data_shutdown_ok = resources.data_if->enable != nullptr &&
                                      resources.device_type != ESP_CODEC_DEV_TYPE_NONE &&
                                      resources.data_if->enable(resources.data_if,
                                                                 resources.device_type, false) == ESP_CODEC_DEV_OK;
        if (!data_shutdown_ok) result.resources_released = false;
    }

    if (resources.device != nullptr) {
        if (resources.device_open) {
            // This second close updates the wrapper's state. Its return value
            // is checked, but the direct codec/data checks above are the
            // authoritative shutdown proof.
            if (esp_codec_dev_close(resources.device) != ESP_CODEC_DEV_OK) {
                result.resources_released = false;
            }
            resources.device_open = false;
        }
        esp_codec_dev_delete(resources.device);
        resources.device = nullptr;
    }
    if (resources.codec_if != nullptr) {
        if (audio_codec_delete_codec_if(resources.codec_if) != ESP_CODEC_DEV_OK) {
            result.resources_released = false;
        }
        resources.codec_if = nullptr;
    }
    if (resources.ctrl_if != nullptr) {
        if (audio_codec_delete_ctrl_if(resources.ctrl_if) != ESP_CODEC_DEV_OK) {
            result.resources_released = false;
        }
        resources.ctrl_if = nullptr;
    }
    if (resources.data_if != nullptr) {
        if (audio_codec_delete_data_if(resources.data_if) != ESP_CODEC_DEV_OK) {
            result.resources_released = false;
        }
        resources.data_if = nullptr;
    }
    if (resources.channel != nullptr) {
        const auto disable_status = i2s_channel_disable(resources.channel);
        if (disable_status != ESP_OK && disable_status != ESP_ERR_INVALID_STATE) {
            result.resources_released = false;
        }
        if (i2s_del_channel(resources.channel) != ESP_OK) {
            result.resources_released = false;
        }
        resources.channel = nullptr;
    }
    result.amplifier_disabled = force_amplifier_off() && result.amplifier_disabled;
    return result;
}

bool acquire_audio_power_lock(esp_pm_lock_handle_t &lock) {
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ng_audio", &lock) != ESP_OK) return false;
    if (esp_pm_lock_acquire(lock) == ESP_OK) return true;
    (void)esp_pm_lock_delete(lock);
    lock = nullptr;
    return false;
}

void release_audio_power_lock(esp_pm_lock_handle_t lock) {
    if (lock == nullptr) return;
    (void)esp_pm_lock_release(lock);
    (void)esp_pm_lock_delete(lock);
}

void generate_test_tone(std::int16_t *samples) {
    constexpr long double kPi = 3.1415926535897932384626433832795L;
    constexpr long double kFrequencyHz = 440.0L;
    for (std::size_t frame = 0; frame < kDiagnosticFrames; ++frame) {
        const auto phase = (2.0L * kPi * kFrequencyHz * static_cast<long double>(frame)) /
                           static_cast<long double>(kSampleRateHz);
        const auto value = static_cast<std::int16_t>(
            std::sin(phase) * static_cast<long double>(kToneAmplitude));
        samples[frame * kChannels] = value;
        samples[(frame * kChannels) + 1] = value;
    }
}

OperationResult run_operation(AudioOperation operation) {
    OperationResult result{};
    AudioResources resources{};
    esp_pm_lock_handle_t pm_lock{nullptr};
    std::uint8_t *buffer{nullptr};

    if (!force_amplifier_off()) {
        result.status = {nightglass::core::StatusCode::io_error, "audio PA low could not be verified"};
        result.amplifier_disabled = false;
        result.resources_released = true;
        return result;
    }
    buffer = static_cast<std::uint8_t *>(
        heap_caps_malloc(kDiagnosticBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (buffer == nullptr) {
        result.status = {nightglass::core::StatusCode::no_memory,
                         "audio DMA buffer unavailable"};
        return result;
    }
    if (!acquire_audio_power_lock(pm_lock)) {
        heap_caps_free(buffer);
        result.status = {nightglass::core::StatusCode::unavailable,
                         "audio power lock unavailable"};
        return result;
    }

    bool opened = false;
    if (operation == AudioOperation::playback) {
        opened = open_output(resources);
        if (opened) {
            mark_operation_active(operation, false, true, true);
            generate_test_tone(reinterpret_cast<std::int16_t *>(buffer));
            if (!set_amplifier(true)) {
                result.status = {nightglass::core::StatusCode::io_error,
                                 "audio PA enable failed"};
            } else {
                portENTER_CRITICAL(&state_mux);
                state.amplifier_disabled = false;
                state.amplifier_disabled_verified = false;
                ++state.sequence;
                portEXIT_CRITICAL(&state_mux);
                std::size_t written{};
                const auto io_status = i2s_channel_write(resources.channel, buffer,
                                                         kDiagnosticBytes, &written,
                                                         kIoTimeoutMs);
                result.transferred_bytes = written;
                if (io_status == ESP_OK && written == kDiagnosticBytes) {
                    result.status = nightglass::core::Status::Ok();
                } else {
                    result.status = {io_status == ESP_ERR_TIMEOUT
                                         ? nightglass::core::StatusCode::timeout
                                         : nightglass::core::StatusCode::io_error,
                                     "speaker transfer incomplete"};
                }
            }
        }
    } else {
        opened = open_input(resources);
        if (opened) {
            mark_operation_active(operation, true, false, true);
            std::size_t read{};
            const auto io_status = i2s_channel_read(resources.channel, buffer,
                                                    kDiagnosticBytes, &read,
                                                    kIoTimeoutMs);
            result.transferred_bytes = read;
            if (io_status == ESP_OK && read == kDiagnosticBytes) {
                result.capture_rms = pcm16_rms(
                    reinterpret_cast<const std::int16_t *>(buffer), read / sizeof(std::int16_t));
                result.status = nightglass::core::Status::Ok();
            } else {
                result.status = {io_status == ESP_ERR_TIMEOUT
                                     ? nightglass::core::StatusCode::timeout
                                     : nightglass::core::StatusCode::io_error,
                                 "microphone transfer incomplete"};
            }
        }
    }

    if (!opened && result.status.is_ok()) {
        result.status = {nightglass::core::StatusCode::io_error,
                         operation == AudioOperation::playback
                             ? "ES8311 output initialization failed"
                             : "ES7210 input initialization failed"};
    }

    const auto cleanup = release_resources(resources);
    heap_caps_free(buffer);
    release_audio_power_lock(pm_lock);
    result.amplifier_disabled = cleanup.amplifier_disabled;
    result.resources_released = cleanup.resources_released;
    if (!cleanup.amplifier_disabled) {
        result.status = {nightglass::core::StatusCode::io_error,
                         "audio PA low could not be verified after test"};
    } else if (!cleanup.resources_released) {
        result.status = {nightglass::core::StatusCode::io_error,
                         "audio cleanup incomplete; restart required"};
    }
    return result;
}

void finish_operation(AudioOperation operation, const OperationResult &result) {
    portENTER_CRITICAL(&state_mux);
    state.hardware_initialized = true;
    state.hardware_active = false;
    state.input_ready = false;
    state.output_ready = false;
    state.operation_pending = false;
    state.operation = operation;
    state.last_operation_ok = result.status.is_ok();
    state.last_operation_status = static_cast<std::uint8_t>(result.status.code);
    state.last_transfer_bytes = static_cast<std::uint32_t>(result.transferred_bytes);
    state.last_io_us = esp_timer_get_time();
    state.amplifier_disabled = result.amplifier_disabled;
    state.amplifier_disabled_verified = result.amplifier_disabled;
    if (operation == AudioOperation::capture) {
        state.frames_captured += static_cast<std::uint32_t>(result.transferred_bytes / kFrameBytes);
        state.last_capture_rms = result.capture_rms;
        if (!result.status.is_ok()) ++state.read_errors;
    } else {
        state.frames_played += static_cast<std::uint32_t>(result.transferred_bytes / kFrameBytes);
        if (!result.status.is_ok()) ++state.write_errors;
    }
    if (result.transferred_bytes < kDiagnosticBytes) {
        state.dropped_bytes += static_cast<std::uint32_t>(kDiagnosticBytes - result.transferred_bytes);
    }
    // A failed transfer can be retried. A missing PA-low proof or a failed
    // channel release cannot, because resource ownership is no longer known.
    state.hardware_failed = !result.amplifier_disabled || !result.resources_released;
    ++state.sequence;
    portEXIT_CRITICAL(&state_mux);

    if (!result.amplifier_disabled) {
        nightglass::core::health_registry().set(
            "audio", nightglass::core::HealthState::failed,
            "PA low unverified; audio diagnostics locked");
    } else if (!result.resources_released) {
        nightglass::core::health_registry().set(
            "audio", nightglass::core::HealthState::failed,
            "audio resource cleanup incomplete; restart required");
    } else if (result.status.is_ok()) {
        nightglass::core::health_registry().set(
            "audio", nightglass::core::HealthState::ok,
            "bounded test complete; PA logic-low verified");
    } else {
        nightglass::core::health_registry().set(
            "audio", nightglass::core::HealthState::degraded,
            "diagnostic transfer failed; PA logic-low verified");
    }
}

void audio_operation_task(void *parameter) {
    const auto operation = static_cast<AudioOperation>(reinterpret_cast<std::uintptr_t>(parameter));
    const auto result = run_operation(operation);
    finish_operation(operation, result);
    vTaskDelete(nullptr);
}

nightglass::core::Status request(AudioOperation operation) {
    if (audio_bus == nullptr) {
        return {nightglass::core::StatusCode::unavailable, "audio service is not armed"};
    }
    portENTER_CRITICAL(&state_mux);
    const bool locked = state.hardware_failed;
    const bool pending = state.operation_pending;
    if (!locked && !pending) {
        state.operation_pending = true;
        state.operation = operation;
        state.last_operation_ok = false;
        ++state.sequence;
    }
    portEXIT_CRITICAL(&state_mux);
    if (locked) {
        return {nightglass::core::StatusCode::unavailable,
                "audio diagnostics locked after a safety failure"};
    }
    if (pending) {
        return {nightglass::core::StatusCode::invalid_state, "audio operation already running"};
    }

    // Resets the user-idle deadline, preventing PowerService's explicit
    // light-sleep path from racing a short diagnostic. The PM lock in the
    // task covers automatic light sleep while hardware is open.
    power_service().note_activity(nightglass::core::WakeReason::touch);
    const auto task_parameter = reinterpret_cast<void *>(static_cast<std::uintptr_t>(operation));
    if (xTaskCreate(audio_operation_task, "ng_audio_test", kAudioTaskStackBytes,
                    task_parameter, 3, nullptr) != pdPASS) {
        portENTER_CRITICAL(&state_mux);
        state.operation_pending = false;
        state.last_operation_status = static_cast<std::uint8_t>(
            nightglass::core::StatusCode::no_memory);
        ++state.sequence;
        portEXIT_CRITICAL(&state_mux);
        return {nightglass::core::StatusCode::no_memory, "audio test worker unavailable"};
    }
    return nightglass::core::Status::Ok();
}
#else
AudioService instance;
AudioSnapshot state{};
#endif

}  // namespace

std::size_t bounded_audio_bytes(std::size_t requested) noexcept {
    constexpr std::size_t kStereoFrameBytes = 2 * sizeof(std::int16_t);
    return std::min(requested, kMaxIoBytes) & ~(kStereoFrameBytes - 1U);
}

std::uint32_t pcm16_rms(const std::int16_t *samples, std::size_t count) noexcept {
    if (samples == nullptr || count == 0) return 0;
    long double sum = 0.0L;
    for (std::size_t i = 0; i < count; ++i) {
        const long double value = samples[i];
        sum += value * value;
    }
    const auto rms = std::sqrt(sum / static_cast<long double>(count));
    return rms >= std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(rms);
}

nightglass::core::Status AudioService::start(i2c_master_bus_handle_t bus_handle) {
#if !NIGHTGLASS_AUDIO_RUNTIME
    (void)bus_handle;
    state = {};
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    state = {};
    state.enabled = true;
    state.amplifier_disabled = force_amplifier_off();
    state.amplifier_disabled_verified = state.amplifier_disabled;
    if (bus_handle == nullptr) {
        state.hardware_failed = true;
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::failed,
                                                 "I2C bus unavailable; audio not armed");
        return {nightglass::core::StatusCode::unavailable, "audio I2C bus unavailable"};
    }
    if (!state.amplifier_disabled) {
        state.hardware_failed = true;
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::failed,
                                                 "PA low could not be verified at boot");
        return {nightglass::core::StatusCode::io_error, "audio PA fail-safe unavailable"};
    }
    audio_bus = bus_handle;
    nightglass::core::health_registry().set(
        "audio", nightglass::core::HealthState::degraded,
        "armed diagnostic only; codec/I2S/PA stay off until explicit test");
    ESP_LOGI(kTag, "Audio armed; PA logic-low verified; hardware deferred");
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status AudioService::request_microphone_sample() {
#if !NIGHTGLASS_AUDIO_RUNTIME
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    return request(AudioOperation::capture);
#endif
}

nightglass::core::Status AudioService::request_test_tone() {
#if !NIGHTGLASS_AUDIO_RUNTIME
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    return request(AudioOperation::playback);
#endif
}

AudioSnapshot AudioService::snapshot() const {
#if NIGHTGLASS_AUDIO_RUNTIME
    AudioSnapshot copy{};
    portENTER_CRITICAL(&state_mux);
    copy = state;
    portEXIT_CRITICAL(&state_mux);
    return copy;
#else
    return state;
#endif
}

AudioService &audio_service() { return instance; }

}  // namespace nightglass::services
