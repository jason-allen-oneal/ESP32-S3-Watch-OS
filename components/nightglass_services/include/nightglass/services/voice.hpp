#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "nightglass/core/status.hpp"
#include "nightglass/services/voice_protocol.hpp"

namespace nightglass::services {

enum class VoiceTurnState : std::uint8_t {
    unavailable,
    idle,
    recording,
    finishing,
    uploading,
    processing,
    complete,
    speaking,
    cancelled,
    failed,
};

enum class VoiceDestination : std::uint8_t {
    openclaw,
    discord_voice_note,
};

// Discord voice-note handoff is intentionally shorter than the general
// OpenClaw recorder so a share attachment stays quick to move and review.
inline constexpr std::uint16_t kVoiceMaximumDiscordReplySeconds = 60;

struct VoiceSettings {
    std::uint16_t maximum_duration_seconds{kVoiceDefaultDurationSeconds};
};

static_assert(std::is_trivially_copyable_v<VoiceSettings>);

constexpr bool valid_voice_settings(const VoiceSettings &settings) noexcept {
    return valid_voice_duration(settings.maximum_duration_seconds);
}

struct VoiceSnapshot {
    std::uint32_t sequence{0};
    VoiceTurnState state{VoiceTurnState::unavailable};
    std::uint32_t session_id{0};
    std::uint32_t recorded_ms{0};
    std::uint32_t encoded_bytes{0};
    std::uint32_t capture_rms{0};
    std::uint16_t response_bytes{0};
    VoiceStatus status{VoiceStatus::ok};
    VoiceHealthState health{VoiceHealthState::unavailable};
    std::uint32_t health_sequence{0};
    std::uint32_t health_age_seconds{0};
    bool spoken_replies{false};
    bool discord_reply{false};
    VoiceSettings settings{};
    std::array<char, kVoiceMaximumResponseBytes + 1> response{};
};

class VoiceService {
public:
    nightglass::core::Status start();
    nightglass::core::Status begin_capture(
        VoiceDestination destination = VoiceDestination::openclaw);
    void finish_capture();
    void cancel();
    void link_lost();
    // Blocks new turns, requests cancellation, and waits until neither the
    // audio worker nor transport worker owns the request buffer.
    [[nodiscard]] bool prepare_for_update(std::uint32_t timeout_ms);
    void update_finished();
    nightglass::core::Status update_settings(const VoiceSettings &settings);
    [[nodiscard]] bool quiescent() const;
    bool accept_frame(const VoiceFrame &frame);
    nightglass::core::Status update_spoken_replies(bool enabled);
    [[nodiscard]] VoiceSnapshot snapshot() const;
};

VoiceService &voice_service();

}  // namespace nightglass::services
