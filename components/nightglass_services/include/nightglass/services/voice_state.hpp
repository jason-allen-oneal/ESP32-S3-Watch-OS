#pragma once

#include <cstdint>

namespace nightglass::services {

enum class VoiceLifecyclePhase : std::uint8_t {
    idle,
    allocating,
    capturing,
    cancelling_capture,
    upload_ready,
    uploading,
    cancelling_upload,
    processing,
    terminal,
};

enum class VoiceBufferOwner : std::uint8_t {
    none,
    audio,
    transport,
};

struct VoiceLifecycle {
    std::uint32_t generation{0};
    VoiceLifecyclePhase phase{VoiceLifecyclePhase::idle};
    VoiceBufferOwner owner{VoiceBufferOwner::none};
    bool cancel_requested{false};
};

[[nodiscard]] bool voice_can_begin(const VoiceLifecycle &state) noexcept;
[[nodiscard]] std::uint32_t voice_reserve_begin(VoiceLifecycle &state) noexcept;
[[nodiscard]] bool voice_capture_queued(VoiceLifecycle &state,
                                        std::uint32_t generation) noexcept;
void voice_begin_failed(VoiceLifecycle &state, std::uint32_t generation) noexcept;
[[nodiscard]] bool voice_request_cancel(VoiceLifecycle &state) noexcept;
[[nodiscard]] bool voice_capture_completed(VoiceLifecycle &state,
                                           std::uint32_t generation,
                                           bool succeeded) noexcept;
[[nodiscard]] bool voice_worker_should_upload(VoiceLifecycle &state,
                                              std::uint32_t generation) noexcept;
[[nodiscard]] bool voice_worker_released(VoiceLifecycle &state,
                                         std::uint32_t generation,
                                         bool upload_succeeded) noexcept;
[[nodiscard]] bool voice_terminal_status_allowed(
    const VoiceLifecycle &state) noexcept;
[[nodiscard]] bool voice_processing_completed(VoiceLifecycle &state) noexcept;
[[nodiscard]] bool voice_is_quiescent(const VoiceLifecycle &state) noexcept;

struct VoiceRetryWindow {
    std::uint32_t acknowledged_offset{0};
    std::uint8_t failures{0};
};

[[nodiscard]] bool voice_note_progress(VoiceRetryWindow &window,
                                       std::uint32_t acknowledged_offset,
                                       std::uint32_t total_bytes) noexcept;
[[nodiscard]] bool voice_retry_after_timeout(VoiceRetryWindow &window,
                                             std::uint8_t maximum_retries) noexcept;
[[nodiscard]] bool voice_deadline_expired(std::int64_t now_us,
                                          std::int64_t deadline_us) noexcept;

}  // namespace nightglass::services
