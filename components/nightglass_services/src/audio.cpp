#include "nightglass/services/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/power.hpp"
#include "nvs.h"
#define NIGHTGLASS_AUDIO_RUNTIME 1
#else
#define NIGHTGLASS_AUDIO_RUNTIME 0
#endif

#if NIGHTGLASS_AUDIO_RUNTIME
#if defined(NIGHTGLASS_SOUND_NOTIFICATION_EMBEDDED)
extern "C" const std::uint8_t ng_notification_start[] asm("_binary_notification_wav_start");
extern "C" const std::uint8_t ng_notification_end[] asm("_binary_notification_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_CALL_EMBEDDED)
extern "C" const std::uint8_t ng_call_start[] asm("_binary_call_wav_start");
extern "C" const std::uint8_t ng_call_end[] asm("_binary_call_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_CALENDAR_EMBEDDED)
extern "C" const std::uint8_t ng_calendar_start[] asm("_binary_calendar_wav_start");
extern "C" const std::uint8_t ng_calendar_end[] asm("_binary_calendar_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_EMAIL_EMBEDDED)
extern "C" const std::uint8_t ng_email_start[] asm("_binary_email_wav_start");
extern "C" const std::uint8_t ng_email_end[] asm("_binary_email_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_ALARM_EMBEDDED)
extern "C" const std::uint8_t ng_alarm_start[] asm("_binary_alarm_wav_start");
extern "C" const std::uint8_t ng_alarm_end[] asm("_binary_alarm_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_TIMER_EMBEDDED)
extern "C" const std::uint8_t ng_timer_start[] asm("_binary_timer_wav_start");
extern "C" const std::uint8_t ng_timer_end[] asm("_binary_timer_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_SUCCESS_EMBEDDED)
extern "C" const std::uint8_t ng_success_start[] asm("_binary_success_wav_start");
extern "C" const std::uint8_t ng_success_end[] asm("_binary_success_wav_end");
#endif
#if defined(NIGHTGLASS_SOUND_WARNING_EMBEDDED)
extern "C" const std::uint8_t ng_warning_start[] asm("_binary_warning_wav_start");
extern "C" const std::uint8_t ng_warning_end[] asm("_binary_warning_wav_end");
#endif
#endif

namespace nightglass::services {

namespace {
std::uint16_t read_le16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_le32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}
}  // namespace

Pcm16WavView parse_pcm16_mono_wav(std::span<const std::uint8_t> sound) noexcept {
    constexpr std::size_t kMaxPcmBytes = 160000;
    constexpr std::uint32_t kExpectedByteRate = kAudioSampleRateHz * sizeof(std::int16_t);
    if (sound.size() < 44 || std::memcmp(sound.data(), "RIFF", 4) != 0 ||
        std::memcmp(sound.data() + 8, "WAVE", 4) != 0 ||
        sound.size() - 8 > std::numeric_limits<std::uint32_t>::max() ||
        read_le32(sound.data() + 4) != sound.size() - 8) {
        return {};
    }

    bool format_seen = false;
    bool format_valid = false;
    bool data_seen = false;
    Pcm16WavView pcm{};
    std::size_t offset = 12;
    while (offset + 8 <= sound.size()) {
        const auto *chunk = sound.data() + offset;
        const auto chunk_size = static_cast<std::size_t>(read_le32(chunk + 4));
        const auto data_offset = offset + 8;
        if (chunk_size > sound.size() - data_offset) return {};
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (format_seen || chunk_size < 16) return {};
            format_seen = true;
            format_valid = read_le16(sound.data() + data_offset) == 1 &&
                           read_le16(sound.data() + data_offset + 2) == 1 &&
                           read_le32(sound.data() + data_offset + 4) == kAudioSampleRateHz &&
                           read_le32(sound.data() + data_offset + 8) == kExpectedByteRate &&
                           read_le16(sound.data() + data_offset + 12) == sizeof(std::int16_t) &&
                           read_le16(sound.data() + data_offset + 14) == 16;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (data_seen) return {};
            data_seen = true;
            pcm = {data_offset, chunk_size, false};
        }
        const auto padded_size = chunk_size + (chunk_size & 1U);
        if (padded_size > sound.size() - data_offset) return {};
        offset = data_offset + padded_size;
    }
    pcm.valid = format_seen && format_valid && data_seen && pcm.data_size > 0 &&
                pcm.data_size <= kMaxPcmBytes &&
                pcm.data_size % sizeof(std::int16_t) == 0;
    return pcm.valid ? pcm : Pcm16WavView{};
}

namespace {

constexpr std::size_t kMaxIoBytes = 4096;

#if NIGHTGLASS_AUDIO_RUNTIME
constexpr char kTag[] = "nightglass_audio";
constexpr char kAudioNvsNamespace[] = "ng_audio";
constexpr std::uint32_t kSampleRateHz = kAudioSampleRateHz;
// Waveshare's maintained board path is mono: one physical speaker and the
// first populated ES7210 capture lane. Keep the transport aligned with that
// proven geometry instead of forcing a stereo slot the board does not use.
constexpr std::uint8_t kChannels = 1;
constexpr std::size_t kFrameBytes = kChannels * sizeof(std::int16_t);
// Stream the audible diagnostic through a small 64 ms PSRAM chunk so its
// temporary allocation coexists with the AMOLED and BLE DMA requirements.
constexpr std::size_t kDiagnosticBytes = 2048;
constexpr std::size_t kDiagnosticFrames = kDiagnosticBytes / kFrameBytes;
constexpr std::uint32_t kIoTimeoutMs = 150;
constexpr std::uint32_t kPlaybackDrainMs = 72;
constexpr std::uint32_t kMicrophoneSettleMs = 50;
constexpr std::uint32_t kAudioTaskStackBytes = 4096;
// Keep two physical queue slots unavailable to ordinary notification/UI cues
// so an alarm and countdown can both be admitted even during a burst. The
// currently playing command is counted separately in pending_count, making
// this reservation conservative while hardware is active.
constexpr UBaseType_t kAudioQueueDepth = 6;
constexpr std::uint8_t kCriticalCueReserve = 2;
constexpr std::int16_t kToneAmplitude = 12000;
// The esp_codec_dev wrapper's volume setter discards the codec driver's I2C
// result. Use the codec interface directly so PA enable is gated on a real
// gain-register write, not merely on the wrapper accepting the request.
constexpr float kMicrophoneGainDb = 30.0F;
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
    std::size_t expected_bytes{kDiagnosticBytes};
    std::uint32_t capture_rms{0};
    bool amplifier_disabled{true};
    bool resources_released{true};
};

struct AudioCommand {
    AudioOperation operation{AudioOperation::none};
    SoundCue cue{SoundCue::test};
};

AudioService instance;
AudioSnapshot state{};
i2c_master_bus_handle_t audio_bus{nullptr};
portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
StaticQueue_t audio_queue_state{};
std::array<std::uint8_t, kAudioQueueDepth * sizeof(AudioCommand)> audio_queue_storage{};
QueueHandle_t audio_queue{nullptr};
TaskHandle_t audio_worker{nullptr};
StaticSemaphore_t request_mutex_state{};
SemaphoreHandle_t request_mutex{nullptr};
std::uint16_t pending_cues{};
std::uint8_t pending_count{};
bool capture_pending{};

AudioSettings load_audio_settings() {
    AudioSettings settings{};
    nvs_handle_t handle{};
    if (nvs_open(kAudioNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    std::uint8_t value{};
    if (nvs_get_u8(handle, "volume", &value) == ESP_OK) settings.volume_percent = value;
    if (nvs_get_u8(handle, "muted", &value) == ESP_OK) settings.muted = value != 0;
    if (nvs_get_u8(handle, "dnd", &value) == ESP_OK) settings.do_not_disturb = value != 0;
    nvs_close(handle);
    return valid_audio_settings(settings) ? settings : AudioSettings{};
}

esp_err_t save_audio_settings(const AudioSettings &settings) {
    nvs_handle_t handle{};
    auto result = nvs_open(kAudioNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    if ((result = nvs_set_u8(handle, "volume", settings.volume_percent)) == ESP_OK &&
        (result = nvs_set_u8(handle, "muted", settings.muted ? 1 : 0)) == ESP_OK &&
        (result = nvs_set_u8(handle, "dnd", settings.do_not_disturb ? 1 : 0)) == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

float output_gain_db(std::uint8_t volume_percent) {
    // Map the full UI range into a conservative codec range whose maximum is
    // the already-proven -8 dB operating point.
    return -40.0F + static_cast<float>(volume_percent) * 0.32F;
}

struct EmbeddedSoundView {
    const std::uint8_t *data{nullptr};
    std::size_t size{0};
};

struct PcmSoundView {
    const std::uint8_t *data{nullptr};
    std::size_t size{0};
};

EmbeddedSoundView embedded_sound(SoundCue cue) {
    switch (cue) {
#if defined(NIGHTGLASS_SOUND_NOTIFICATION_EMBEDDED)
        case SoundCue::notification:
            return {ng_notification_start,
                    static_cast<std::size_t>(ng_notification_end - ng_notification_start)};
#endif
#if defined(NIGHTGLASS_SOUND_CALL_EMBEDDED)
        case SoundCue::call:
            return {ng_call_start, static_cast<std::size_t>(ng_call_end - ng_call_start)};
#endif
#if defined(NIGHTGLASS_SOUND_CALENDAR_EMBEDDED)
        case SoundCue::calendar:
            return {ng_calendar_start,
                    static_cast<std::size_t>(ng_calendar_end - ng_calendar_start)};
#endif
#if defined(NIGHTGLASS_SOUND_EMAIL_EMBEDDED)
        case SoundCue::email:
            return {ng_email_start, static_cast<std::size_t>(ng_email_end - ng_email_start)};
#endif
#if defined(NIGHTGLASS_SOUND_ALARM_EMBEDDED)
        case SoundCue::alarm:
            return {ng_alarm_start, static_cast<std::size_t>(ng_alarm_end - ng_alarm_start)};
#endif
#if defined(NIGHTGLASS_SOUND_TIMER_EMBEDDED)
        case SoundCue::timer:
            return {ng_timer_start, static_cast<std::size_t>(ng_timer_end - ng_timer_start)};
#endif
#if defined(NIGHTGLASS_SOUND_SUCCESS_EMBEDDED)
        case SoundCue::success:
            return {ng_success_start,
                    static_cast<std::size_t>(ng_success_end - ng_success_start)};
#endif
#if defined(NIGHTGLASS_SOUND_WARNING_EMBEDDED)
        case SoundCue::warning:
            return {ng_warning_start,
                    static_cast<std::size_t>(ng_warning_end - ng_warning_start)};
#endif
        default: return {};
    }
}

PcmSoundView parse_embedded_wav(EmbeddedSoundView sound) {
    if (sound.data == nullptr) return {};
    const auto parsed = parse_pcm16_mono_wav({sound.data, sound.size});
    return parsed.valid
               ? PcmSoundView{sound.data + parsed.data_offset, parsed.data_size}
               : PcmSoundView{};
}

bool set_amplifier(bool enabled) {
    if (gpio_set_direction(kAmplifierGpio, GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
        gpio_set_level(kAmplifierGpio, enabled ? 1 : 0) != ESP_OK) {
        return false;
    }
    return gpio_get_level(kAmplifierGpio) == (enabled ? 1 : 0);
}

bool force_amplifier_off() { return set_amplifier(false); }

void mark_operation_active(AudioOperation operation, SoundCue cue, bool input, bool output,
                           bool amplifier_disabled, std::uint8_t volume_percent) {
    portENTER_CRITICAL(&state_mux);
    state.hardware_initialized = true;
    state.hardware_active = true;
    state.input_ready = input;
    state.output_ready = output;
    state.operation = operation;
    state.cue = cue;
    state.output_volume = output ? volume_percent : 0;
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
                                                         I2S_SLOT_MODE_MONO),
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

bool open_output(AudioResources &resources, std::uint8_t volume_percent) {
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
        .channel_mask = 0x01,
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
        resources.codec_if->set_vol(resources.codec_if, output_gain_db(volume_percent)) !=
            ESP_CODEC_DEV_OK) {
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
        .channel_mask = 0x01,
        .sample_rate = kSampleRateHz,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(resources.device, &sample) != ESP_CODEC_DEV_OK) return false;
    resources.device_open = true;
    // esp_codec_dev_open() reapplies its zero-initialized cached gain after
    // the ES7210 driver selects a sane 30 dB default. Restore the diagnostic
    // gain through the underlying interface and require the I2C result.
    if (resources.codec_if->set_mic_gain == nullptr ||
        resources.codec_if->set_mic_gain(resources.codec_if, kMicrophoneGainDb) !=
            ESP_CODEC_DEV_OK) {
        return false;
    }
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
        bool codec_shutdown_ok = true;
        if (resources.device_open) {
            const int shutdown_status = resources.codec_if->enable != nullptr
                                            ? resources.codec_if->enable(resources.codec_if, false)
                                            : ESP_CODEC_DEV_NOT_SUPPORT;
            codec_shutdown_ok = shutdown_status == ESP_CODEC_DEV_OK;
            if (!codec_shutdown_ok) {
                ESP_LOGE(kTag, "codec shutdown failed: %d", shutdown_status);
            }
        }
        if (!codec_shutdown_ok) result.resources_released = false;
    }
    if (resources.device != nullptr) {
        if (resources.device_open) {
            // This close updates the wrapper's state and requests I2S stop.
            // The explicit channel disable below proves the final driver
            // state because the wrapper discards its data-interface result.
            if (esp_codec_dev_close(resources.device) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(kTag, "codec wrapper close failed");
                result.resources_released = false;
            }
            resources.device_open = false;
        }
        esp_codec_dev_delete(resources.device);
        resources.device = nullptr;
    }
    if (resources.codec_if != nullptr) {
        const int delete_status = audio_codec_delete_codec_if(resources.codec_if);
        if (delete_status != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "codec interface delete failed: %d", delete_status);
            result.resources_released = false;
        }
        resources.codec_if = nullptr;
    }
    if (resources.ctrl_if != nullptr) {
        const int delete_status = audio_codec_delete_ctrl_if(resources.ctrl_if);
        if (delete_status != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "codec control delete failed: %d", delete_status);
            result.resources_released = false;
        }
        resources.ctrl_if = nullptr;
    }
    if (resources.data_if != nullptr) {
        const int delete_status = audio_codec_delete_data_if(resources.data_if);
        if (delete_status != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "codec data delete failed: %d", delete_status);
            result.resources_released = false;
        }
        resources.data_if = nullptr;
    }
    if (resources.channel != nullptr) {
        const auto disable_status = i2s_channel_disable(resources.channel);
        if (disable_status != ESP_OK && disable_status != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(kTag, "I2S disable failed: %s", esp_err_to_name(disable_status));
            result.resources_released = false;
        }
        const auto delete_status = i2s_del_channel(resources.channel);
        if (delete_status != ESP_OK) {
            ESP_LOGE(kTag, "I2S channel delete failed: %s", esp_err_to_name(delete_status));
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

bool release_audio_power_lock(esp_pm_lock_handle_t lock) {
    if (lock == nullptr) return true;
    const bool released = esp_pm_lock_release(lock) == ESP_OK;
    const bool deleted = esp_pm_lock_delete(lock) == ESP_OK;
    return released && deleted;
}

long double cue_frequency(SoundCue cue, std::uint32_t elapsed_ms) {
    switch (cue) {
        case SoundCue::notification:
            if (elapsed_ms < 160) return 880.0L;
            if (elapsed_ms < 220) return 0.0L;
            return 1320.0L;
        case SoundCue::call:
            return elapsed_ms % 250U < 200U
                       ? ((elapsed_ms / 250U) % 2U ? 988.0L : 740.0L)
                       : 0.0L;
        case SoundCue::calendar:
            if (elapsed_ms < 250) return 784.0L;
            if (elapsed_ms < 330) return 0.0L;
            return 1175.0L;
        case SoundCue::email:
            if (elapsed_ms < 200) return 660.0L;
            if (elapsed_ms < 260) return 0.0L;
            return 880.0L;
        case SoundCue::alarm:
            return (elapsed_ms / 250U) % 2U ? 660.0L : 880.0L;
        case SoundCue::timer:
            return elapsed_ms % 300U < 160U ? 1200.0L : 0.0L;
        case SoundCue::success:
            return elapsed_ms < 250U ? 880.0L : 1320.0L;
        case SoundCue::warning:
            return elapsed_ms % 400U < 250U ? 440.0L : 0.0L;
        case SoundCue::test:
            return 880.0L;
    }
    return 880.0L;
}

void generate_sound_cue(std::int16_t *samples, SoundCue cue,
                        std::size_t initial_frame, std::size_t frame_count) {
    constexpr long double kPi = 3.1415926535897932384626433832795L;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto absolute_frame = initial_frame + frame;
        const auto elapsed_ms = static_cast<std::uint32_t>(
            absolute_frame * 1000U / kSampleRateHz);
        const auto frequency_hz = cue_frequency(cue, elapsed_ms);
        const auto phase = (2.0L * kPi * frequency_hz *
                            static_cast<long double>(absolute_frame)) /
                           static_cast<long double>(kSampleRateHz);
        const auto value = frequency_hz == 0.0L
                               ? std::int16_t{0}
                               : static_cast<std::int16_t>(
                                     std::sin(phase) * static_cast<long double>(kToneAmplitude));
        samples[frame * kChannels] = value;
    }
}

OperationResult run_operation(AudioOperation operation, SoundCue cue) {
    OperationResult result{};
    const auto embedded = operation == AudioOperation::playback
                              ? embedded_sound(cue)
                              : EmbeddedSoundView{};
    const auto pcm = parse_embedded_wav(embedded);
    if (embedded.data != nullptr && pcm.data == nullptr) {
        ESP_LOGW(kTag, "Embedded sound %u invalid; using generated fallback",
                 static_cast<unsigned>(cue));
    }
    result.expected_bytes = operation == AudioOperation::capture
                                ? kDiagnosticBytes
                                : pcm.data != nullptr
                                      ? pcm.size
                                      : sound_cue_transfer_bytes(cue);
    AudioResources resources{};
    esp_pm_lock_handle_t pm_lock{nullptr};
    std::uint8_t *buffer{nullptr};
    AudioSettings audio_settings{};
    portENTER_CRITICAL(&state_mux);
    audio_settings = state.settings;
    portEXIT_CRITICAL(&state_mux);

    if (!force_amplifier_off()) {
        result.status = {nightglass::core::StatusCode::io_error, "audio PA low could not be verified"};
        result.amplifier_disabled = false;
        result.resources_released = true;
        return result;
    }
    // The I2S driver copies between this application buffer and its own DMA
    // ring. Keep samples in PSRAM so AMOLED, BLE, and DMA descriptors retain
    // scarce contiguous internal memory.
    buffer = static_cast<std::uint8_t *>(
        heap_caps_malloc(kDiagnosticBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
        opened = open_output(resources, audio_settings.volume_percent);
        if (opened) {
            mark_operation_active(operation, cue, false, true, true,
                                  audio_settings.volume_percent);
            // Prove the enabled I2S path accepts data while the physical PA is
            // still low. esp_codec_dev_open() does not propagate every data-if
            // error from the vendor wrapper.
            std::fill_n(buffer, kFrameBytes, 0U);
            std::size_t probe_written{};
            const auto probe_status = i2s_channel_write(resources.channel, buffer,
                                                         kFrameBytes, &probe_written,
                                                         kIoTimeoutMs);
            if (probe_status != ESP_OK || probe_written != kFrameBytes) {
                result.status = {probe_status == ESP_ERR_TIMEOUT
                                     ? nightglass::core::StatusCode::timeout
                                     : nightglass::core::StatusCode::io_error,
                                 "speaker path readiness probe failed"};
            } else if (!set_amplifier(true)) {
                result.status = {nightglass::core::StatusCode::io_error,
                                 "audio PA enable failed"};
            } else {
                portENTER_CRITICAL(&state_mux);
                state.amplifier_disabled = false;
                state.amplifier_disabled_verified = false;
                ++state.sequence;
                portEXIT_CRITICAL(&state_mux);
                bool transfer_ok = true;
                std::size_t frame_offset{};
                const auto cue_frames = result.expected_bytes / kFrameBytes;
                while (frame_offset < cue_frames) {
                    const auto frames_this_chunk =
                        std::min(kDiagnosticFrames, cue_frames - frame_offset);
                    const auto bytes_this_chunk = frames_this_chunk * kFrameBytes;
                    if (pcm.data != nullptr) {
                        std::memcpy(buffer, pcm.data + frame_offset * kFrameBytes,
                                    bytes_this_chunk);
                    } else {
                        generate_sound_cue(reinterpret_cast<std::int16_t *>(buffer), cue,
                                           frame_offset, frames_this_chunk);
                    }
                    std::size_t written{};
                    const auto io_status = i2s_channel_write(
                        resources.channel, buffer, bytes_this_chunk, &written, kIoTimeoutMs);
                    result.transferred_bytes += written;
                    if (io_status != ESP_OK || written != bytes_this_chunk) {
                        result.status = {io_status == ESP_ERR_TIMEOUT
                                             ? nightglass::core::StatusCode::timeout
                                             : nightglass::core::StatusCode::io_error,
                                         "speaker transfer incomplete"};
                        transfer_ok = false;
                        break;
                    }
                    frame_offset += frames_this_chunk;
                }
                if (transfer_ok && result.transferred_bytes == result.expected_bytes) {
                    // A successful write means the bytes reached the DMA ring,
                    // not that the final sample has left the I2S peripheral.
                    // Keep the PM lock and physical PA gate active until the
                    // final streamed chunk has left the I2S peripheral.
                    vTaskDelay(pdMS_TO_TICKS(kPlaybackDrainMs));
                    result.status = nightglass::core::Status::Ok();
                }
            }
        }
    } else {
        opened = open_input(resources);
        if (opened) {
            mark_operation_active(operation, cue, true, false, true, 0);
            // Let the ES7210 bias/HPF settle and allow its two-buffer DMA ring
            // to overwrite startup zeros before measuring the live capsules.
            vTaskDelay(pdMS_TO_TICKS(kMicrophoneSettleMs));
            std::size_t read{};
            const auto io_status = i2s_channel_read(resources.channel, buffer,
                                                    kDiagnosticBytes, &read,
                                                    kIoTimeoutMs);
            result.transferred_bytes = read;
            if (io_status == ESP_OK && read == kDiagnosticBytes) {
                result.capture_rms = pcm16_rms(
                    reinterpret_cast<const std::int16_t *>(buffer), read / sizeof(std::int16_t));
#if CONFIG_NIGHTGLASS_AUDIO_BOOT_SELF_TEST
                const auto *samples = reinterpret_cast<const std::int16_t *>(buffer);
                std::int16_t minimum = samples[0];
                std::int16_t maximum = samples[0];
                std::size_t nonzero{};
                for (std::size_t index = 0; index < read / sizeof(std::int16_t); ++index) {
                    minimum = std::min(minimum, samples[index]);
                    maximum = std::max(maximum, samples[index]);
                    if (samples[index] != 0) ++nonzero;
                }
                ESP_LOGI(kTag, "capture raw min=%d max=%d nonzero=%u/%u",
                         minimum, maximum, static_cast<unsigned>(nonzero),
                         static_cast<unsigned>(read / sizeof(std::int16_t)));
#endif
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
    const bool power_lock_released = release_audio_power_lock(pm_lock);
    if (!power_lock_released) ESP_LOGE(kTag, "audio PM lock cleanup failed");
    result.amplifier_disabled = cleanup.amplifier_disabled;
    result.resources_released = cleanup.resources_released && power_lock_released;
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
    if (result.transferred_bytes < result.expected_bytes) {
        state.dropped_bytes +=
            static_cast<std::uint32_t>(result.expected_bytes - result.transferred_bytes);
    }
    // A failed transfer can be retried. A missing PA-low proof or a failed
    // channel release cannot, because resource ownership is no longer known.
    state.hardware_failed = state.hardware_failed || !result.amplifier_disabled ||
                            !result.resources_released;
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

void audio_worker_task(void *) {
    while (true) {
        AudioCommand command{};
        if (xQueueReceive(audio_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        portENTER_CRITICAL(&state_mux);
        const bool locked = state.hardware_failed;
        auto effective_settings = state.settings;
        effective_settings.do_not_disturb = effective_settings.do_not_disturb ||
                                             state.scheduled_dnd;
        const bool allowed = command.operation != AudioOperation::playback ||
                             audio_cue_allowed(command.cue, effective_settings);
        if (locked) {
            state.operation = command.operation;
            state.cue = command.cue;
            state.last_operation_ok = false;
            state.last_operation_status = static_cast<std::uint8_t>(
                nightglass::core::StatusCode::unavailable);
            ++state.sequence;
        }
        portEXIT_CRITICAL(&state_mux);
        if (!locked && allowed) {
            const auto result = run_operation(command.operation, command.cue);
            finish_operation(command.operation, result);
        }
        portENTER_CRITICAL(&state_mux);
        const bool safety_locked = state.hardware_failed;
        portEXIT_CRITICAL(&state_mux);
        if (safety_locked) {
            // Once PA-low or resource ownership is uncertain, no queued
            // command may attempt another codec/I2S session. New requests are
            // rejected by request(); reset the existing fixed queue and its
            // mirrors immediately rather than walking stale commands.
            (void)xQueueReset(audio_queue);
            portENTER_CRITICAL(&state_mux);
            pending_cues = 0;
            pending_count = 0;
            capture_pending = false;
            state.operation_pending = false;
            ++state.sequence;
            portEXIT_CRITICAL(&state_mux);
            continue;
        }
        portENTER_CRITICAL(&state_mux);
        if (command.operation == AudioOperation::capture) {
            capture_pending = false;
        } else {
            pending_cues &= static_cast<std::uint16_t>(
                ~(1U << static_cast<unsigned>(command.cue)));
        }
        if (pending_count > 0) --pending_count;
        state.operation_pending = pending_count > 0;
        ++state.sequence;
        portEXIT_CRITICAL(&state_mux);
    }
}

nightglass::core::Status request(AudioOperation operation, SoundCue cue,
                                 nightglass::core::WakeReason wake_reason) {
    if (audio_bus == nullptr || audio_queue == nullptr || audio_worker == nullptr ||
        request_mutex == nullptr) {
        return {nightglass::core::StatusCode::unavailable, "audio service is not armed"};
    }
    if (xSemaphoreTake(request_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return {nightglass::core::StatusCode::unavailable,
                "audio request path is busy"};
    }
    const auto cue_bit = static_cast<std::uint16_t>(
        1U << static_cast<unsigned>(cue));
    const bool critical = operation == AudioOperation::playback &&
                          (cue == SoundCue::alarm || cue == SoundCue::timer);
    portENTER_CRITICAL(&state_mux);
    const bool locked = state.hardware_failed;
    const bool duplicate = operation == AudioOperation::capture
                               ? capture_pending
                               : (pending_cues & cue_bit) != 0;
    const bool reserved_capacity = !critical &&
                                   pending_count >=
                                       kAudioQueueDepth - kCriticalCueReserve;
    portEXIT_CRITICAL(&state_mux);
    if (locked) {
        xSemaphoreGive(request_mutex);
        return {nightglass::core::StatusCode::unavailable,
                "audio diagnostics locked after a safety failure"};
    }
    if (duplicate) {
        xSemaphoreGive(request_mutex);
        return nightglass::core::Status::Ok();
    }
    if (reserved_capacity) {
        portENTER_CRITICAL(&state_mux);
        state.last_operation_status = static_cast<std::uint8_t>(
            nightglass::core::StatusCode::no_memory);
        ++state.sequence;
        portEXIT_CRITICAL(&state_mux);
        xSemaphoreGive(request_mutex);
        return {nightglass::core::StatusCode::no_memory,
                "audio cue queue reserved for alarm/timer"};
    }

    // Resets the user-idle deadline, preventing PowerService's explicit
    // light-sleep path from racing a short diagnostic. The PM lock in the
    // task covers automatic light sleep while hardware is open.
    power_service().note_activity(wake_reason);
    const AudioCommand command{operation, cue};
    const bool urgent = critical || cue == SoundCue::call;
    const auto queued = urgent ? xQueueSendToFront(audio_queue, &command, 0)
                               : xQueueSendToBack(audio_queue, &command, 0);
    if (queued != pdTRUE) {
        portENTER_CRITICAL(&state_mux);
        state.last_operation_status = static_cast<std::uint8_t>(
            nightglass::core::StatusCode::no_memory);
        ++state.sequence;
        portEXIT_CRITICAL(&state_mux);
        xSemaphoreGive(request_mutex);
        return {nightglass::core::StatusCode::no_memory, "audio cue queue full"};
    }
    // Publish duplicate-suppression state only after the command is actually
    // present in the queue. The request mutex prevents a concurrent caller
    // from observing a reservation that a failed send later rolls back.
    portENTER_CRITICAL(&state_mux);
    if (operation == AudioOperation::capture) capture_pending = true;
    else pending_cues |= cue_bit;
    ++pending_count;
    state.operation_pending = true;
    state.operation = operation;
    state.cue = cue;
    state.last_operation_ok = false;
    ++state.sequence;
    portEXIT_CRITICAL(&state_mux);
    xSemaphoreGive(request_mutex);
    return nightglass::core::Status::Ok();
}
#else
AudioService instance;
AudioSnapshot state{};
#endif

}  // namespace

std::size_t bounded_audio_bytes(std::size_t requested) noexcept {
    constexpr std::size_t kMonoFrameBytes = sizeof(std::int16_t);
    return std::min(requested, kMaxIoBytes) & ~(kMonoFrameBytes - 1U);
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
    const auto persisted_settings = load_audio_settings();
    state = {};
    state.settings = persisted_settings;
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
    pending_cues = 0;
    pending_count = 0;
    capture_pending = false;
    request_mutex = xSemaphoreCreateMutexStatic(&request_mutex_state);
    audio_queue = xQueueCreateStatic(kAudioQueueDepth, sizeof(AudioCommand),
                                     audio_queue_storage.data(), &audio_queue_state);
    if (request_mutex == nullptr || audio_queue == nullptr ||
        xTaskCreateWithCaps(audio_worker_task, "ng_audio", kAudioTaskStackBytes,
                            nullptr, 3, &audio_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        audio_queue = nullptr;
        audio_worker = nullptr;
        state.hardware_failed = true;
        nightglass::core::health_registry().set(
            "audio", nightglass::core::HealthState::failed,
            "audio command worker unavailable");
        return {nightglass::core::StatusCode::no_memory, "audio command worker unavailable"};
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
    return request(AudioOperation::capture, SoundCue::test,
                   nightglass::core::WakeReason::touch);
#endif
}

nightglass::core::Status AudioService::request_test_tone() {
#if !NIGHTGLASS_AUDIO_RUNTIME
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    return request_sound(SoundCue::test);
#endif
}

nightglass::core::Status AudioService::request_sound(SoundCue cue) {
#if !NIGHTGLASS_AUDIO_RUNTIME
    (void)cue;
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    AudioSettings settings{};
    portENTER_CRITICAL(&state_mux);
    settings = state.settings;
    settings.do_not_disturb = settings.do_not_disturb || state.scheduled_dnd;
    portEXIT_CRITICAL(&state_mux);
    if (!audio_cue_allowed(cue, settings)) {
        return nightglass::core::Status::Ok();
    }
    auto wake_reason = nightglass::core::WakeReason::notification;
    if (cue == SoundCue::alarm) wake_reason = nightglass::core::WakeReason::alarm;
    else if (cue == SoundCue::timer) wake_reason = nightglass::core::WakeReason::timer;
    else if (cue == SoundCue::test || cue == SoundCue::success || cue == SoundCue::warning) {
        wake_reason = nightglass::core::WakeReason::touch;
    }
    return request(AudioOperation::playback, cue, wake_reason);
#endif
}

void AudioService::set_scheduled_dnd(bool active) {
#if NIGHTGLASS_AUDIO_RUNTIME
    portENTER_CRITICAL(&state_mux);
    if (state.scheduled_dnd != active) {
        state.scheduled_dnd = active;
        ++state.sequence;
    }
    portEXIT_CRITICAL(&state_mux);
#else
    (void)active;
#endif
}

nightglass::core::Status AudioService::update_settings(const AudioSettings &settings) {
#if !NIGHTGLASS_AUDIO_RUNTIME
    (void)settings;
    return {nightglass::core::StatusCode::unavailable,
            "audio disabled by build configuration"};
#else
    if (!valid_audio_settings(settings)) {
        return {nightglass::core::StatusCode::invalid_state, "invalid audio settings"};
    }
    const auto persisted = save_audio_settings(settings);
    if (persisted != ESP_OK) {
        return {nightglass::core::StatusCode::io_error,
                "audio settings persistence failed"};
    }
    portENTER_CRITICAL(&state_mux);
    state.settings = settings;
    ++state.sequence;
    portEXIT_CRITICAL(&state_mux);
    return nightglass::core::Status::Ok();
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
