#include "nightglass/services/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#if defined(CONFIG_NIGHTGLASS_AUDIO_ENABLED) && CONFIG_NIGHTGLASS_AUDIO_ENABLED
#include "esp_log.h"
#include "esp_timer.h"
#include "nightglass/core/health.hpp"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#define NIGHTGLASS_AUDIO_RUNTIME 1
#else
#define NIGHTGLASS_AUDIO_RUNTIME 0
#endif

#if __has_include("freertos/FreeRTOS.h")
#include "nightglass/core/health.hpp"
#define NIGHTGLASS_AUDIO_HAS_HEALTH 1
#else
#define NIGHTGLASS_AUDIO_HAS_HEALTH 0
#endif

namespace nightglass::services {
namespace {
constexpr std::size_t kMaxIoBytes = 4096;
#if NIGHTGLASS_AUDIO_RUNTIME
constexpr char kTag[] = "nightglass_audio";
constexpr std::uint32_t kSampleRateHz = 16000;
constexpr std::uint8_t kChannels = 2;
constexpr std::uint8_t kTestToneVolume = 60;
constexpr std::size_t kTestToneFrames = kMaxIoBytes / (kChannels * sizeof(std::int16_t));
std::array<std::int16_t, kTestToneFrames * kChannels> test_tone_samples{};
#endif

AudioService instance;
AudioSnapshot state{};

#if NIGHTGLASS_AUDIO_RUNTIME
esp_codec_dev_handle_t input = nullptr;
esp_codec_dev_handle_t output = nullptr;
bool started = false;
#endif
}  // namespace

std::size_t bounded_audio_bytes(std::size_t requested) noexcept {
    return std::min(requested, kMaxIoBytes);
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
#if NIGHTGLASS_AUDIO_HAS_HEALTH
    nightglass::core::health_registry().set("audio", nightglass::core::HealthState::absent,
                                             "disabled by build configuration; no audio hardware touched");
#endif
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    if (started) return nightglass::core::Status::Ok();
    if (bus_handle == nullptr) {
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::failed,
                                                 "I2C bus unavailable");
        return {nightglass::core::StatusCode::unavailable, "audio I2C bus unavailable"};
    }

    // The Waveshare BSP owns the physical pins and codec interfaces. Override
    // only the sample format; no audio task or unbounded queue is created.
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    if (bsp_audio_init(&i2s_config) != ESP_OK) {
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::failed,
                                                 "I2S initialization failed");
        return {nightglass::core::StatusCode::io_error, "audio I2S initialization failed"};
    }
    output = bsp_audio_codec_speaker_init();
    input = bsp_audio_codec_microphone_init();
    if (output == nullptr || input == nullptr) {
        if (output != nullptr) (void)esp_codec_dev_close(output);
        if (input != nullptr) (void)esp_codec_dev_close(input);
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::degraded,
                                                 "ES8311/ES7210 codec handle unavailable; output/input not opened");
        return {nightglass::core::StatusCode::io_error, "audio codec initialization failed"};
    }
    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = kChannels,
        .channel_mask = 0x03,
        .sample_rate = kSampleRateHz,
        .mclk_multiple = 256,
    };
    const auto output_opened = esp_codec_dev_open(output, &sample) == ESP_CODEC_DEV_OK;
    const auto output_muted = output_opened &&
                              esp_codec_dev_set_out_mute(output, true) == ESP_CODEC_DEV_OK;
    const auto input_opened = esp_codec_dev_open(input, &sample) == ESP_CODEC_DEV_OK;
    const auto volume_set = output_opened &&
                            esp_codec_dev_set_out_vol(output, kTestToneVolume) == ESP_CODEC_DEV_OK;
    if (!output_opened || !output_muted || !input_opened || !volume_set) {
        if (output != nullptr) (void)esp_codec_dev_close(output);
        if (input != nullptr) (void)esp_codec_dev_close(input);
        nightglass::core::health_registry().set("audio", nightglass::core::HealthState::failed,
                                                 "codec open failed; ES8311 output shutdown attempted");
        return {nightglass::core::StatusCode::io_error, "audio codec open failed"};
    }
    started = true;
    state.enabled = true;
    state.input_ready = true;
    state.output_ready = true;
    state.output_muted = true;
    state.output_volume = kTestToneVolume;
    nightglass::core::health_registry().set("audio", nightglass::core::HealthState::ok,
                                             "ES8311 output / ES7210 input ready; output muted");
    ESP_LOGI(kTag, "Audio ready at %u Hz, %u channels", kSampleRateHz, kChannels);
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status AudioService::capture(std::span<std::uint8_t> destination) {
#if !NIGHTGLASS_AUDIO_RUNTIME
    (void)destination;
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    const auto bytes = bounded_audio_bytes(destination.size());
    if (!started || input == nullptr || bytes == 0 || (bytes % 4) != 0) {
        return {nightglass::core::StatusCode::invalid_state, "capture unavailable or invalid buffer"};
    }
    if (esp_codec_dev_read(input, destination.data(), static_cast<int>(bytes)) != ESP_CODEC_DEV_OK) {
        ++state.read_errors;
        return {nightglass::core::StatusCode::io_error, "audio capture failed"};
    }
    state.frames_captured += static_cast<std::uint32_t>(bytes / (2 * sizeof(std::int16_t)));
    state.last_io_us = esp_timer_get_time();
    if (destination.size() > bytes) {
        state.dropped_bytes += static_cast<std::uint32_t>(destination.size() - bytes);
    }
    return nightglass::core::Status::Ok();
#endif
}

nightglass::core::Status AudioService::play(std::span<const std::uint8_t> samples) {
#if !NIGHTGLASS_AUDIO_RUNTIME
    (void)samples;
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    const auto bytes = bounded_audio_bytes(samples.size());
    if (!started || output == nullptr || bytes == 0 || (bytes % 4) != 0) {
        return {nightglass::core::StatusCode::invalid_state, "playback unavailable or invalid buffer"};
    }
    if (esp_codec_dev_write(output, const_cast<std::uint8_t *>(samples.data()),
                            static_cast<int>(bytes)) != ESP_CODEC_DEV_OK) {
        ++state.write_errors;
        return {nightglass::core::StatusCode::io_error, "audio playback failed"};
    }
    state.frames_played += static_cast<std::uint32_t>(bytes / (2 * sizeof(std::int16_t)));
    state.last_io_us = esp_timer_get_time();
    if (samples.size() > bytes) state.dropped_bytes += static_cast<std::uint32_t>(samples.size() - bytes);
    return nightglass::core::Status::Ok();
#endif
}

AudioSnapshot AudioService::snapshot() const { return state; }

nightglass::core::Status AudioService::play_test_tone() {
#if !NIGHTGLASS_AUDIO_RUNTIME
    return {nightglass::core::StatusCode::unavailable, "audio disabled by build configuration"};
#else
    if (!started || output == nullptr) {
        return {nightglass::core::StatusCode::invalid_state, "audio output unavailable"};
    }

    constexpr long double kPi = 3.1415926535897932384626433832795L;
    constexpr long double kFrequencyHz = 440.0L;
    constexpr std::int16_t kAmplitude = 6000;
    for (std::size_t frame = 0; frame < kTestToneFrames; ++frame) {
        const auto phase = (2.0L * kPi * kFrequencyHz * static_cast<long double>(frame)) /
                           static_cast<long double>(kSampleRateHz);
        const auto value = static_cast<std::int16_t>(
            std::sin(phase) * static_cast<long double>(kAmplitude));
        test_tone_samples[frame * kChannels] = value;
        test_tone_samples[(frame * kChannels) + 1] = value;
    }

    if (esp_codec_dev_set_out_vol(output, kTestToneVolume) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(output, false) != ESP_CODEC_DEV_OK) {
        ++state.write_errors;
        state.output_muted = true;
        (void)esp_codec_dev_set_out_mute(output, true);
        return {nightglass::core::StatusCode::io_error, "audio output enable failed"};
    }
    state.output_muted = false;

    const auto write_result = esp_codec_dev_write(output, test_tone_samples.data(),
                                                   static_cast<int>(sizeof(test_tone_samples)));
    const auto mute_result = esp_codec_dev_set_out_mute(output, true);
    state.output_muted = true;
    if (write_result != ESP_CODEC_DEV_OK) {
        ++state.write_errors;
        return {nightglass::core::StatusCode::io_error, "audio test tone failed"};
    }
    if (mute_result != ESP_CODEC_DEV_OK) {
        ++state.write_errors;
        return {nightglass::core::StatusCode::io_error, "audio output mute failed"};
    }
    state.frames_played += static_cast<std::uint32_t>(kTestToneFrames);
    state.last_io_us = esp_timer_get_time();
    return nightglass::core::Status::Ok();
#endif
}

AudioService &audio_service() { return instance; }

}  // namespace nightglass::services
