#include "nightglass/services/voice_state.hpp"

namespace nightglass::services {

bool voice_can_begin(const VoiceLifecycle &state) noexcept {
    return state.owner == VoiceBufferOwner::none &&
           (state.phase == VoiceLifecyclePhase::idle ||
            state.phase == VoiceLifecyclePhase::terminal);
}

std::uint32_t voice_reserve_begin(VoiceLifecycle &state) noexcept {
    if (!voice_can_begin(state)) return 0;
    if (++state.generation == 0) ++state.generation;
    state.phase = VoiceLifecyclePhase::allocating;
    state.owner = VoiceBufferOwner::none;
    state.cancel_requested = false;
    return state.generation;
}

bool voice_capture_queued(VoiceLifecycle &state, std::uint32_t generation) noexcept {
    if (generation == 0 || generation != state.generation ||
        state.phase != VoiceLifecyclePhase::allocating ||
        state.owner != VoiceBufferOwner::none) {
        return false;
    }
    state.phase = VoiceLifecyclePhase::capturing;
    state.owner = VoiceBufferOwner::audio;
    return true;
}

void voice_begin_failed(VoiceLifecycle &state, std::uint32_t generation) noexcept {
    if (generation != state.generation || state.phase != VoiceLifecyclePhase::allocating) return;
    state.phase = VoiceLifecyclePhase::terminal;
    state.owner = VoiceBufferOwner::none;
}

bool voice_request_cancel(VoiceLifecycle &state) noexcept {
    state.cancel_requested = true;
    switch (state.phase) {
        case VoiceLifecyclePhase::allocating:
            state.phase = VoiceLifecyclePhase::terminal;
            return true;
        case VoiceLifecyclePhase::capturing:
            state.phase = VoiceLifecyclePhase::cancelling_capture;
            return true;
        case VoiceLifecyclePhase::upload_ready:
        case VoiceLifecyclePhase::uploading:
            state.phase = VoiceLifecyclePhase::cancelling_upload;
            return true;
        case VoiceLifecyclePhase::processing:
            state.phase = VoiceLifecyclePhase::terminal;
            return true;
        case VoiceLifecyclePhase::cancelling_capture:
        case VoiceLifecyclePhase::cancelling_upload:
            return true;
        case VoiceLifecyclePhase::idle:
        case VoiceLifecyclePhase::terminal:
            return false;
    }
    return false;
}

bool voice_capture_completed(VoiceLifecycle &state, std::uint32_t generation,
                             bool succeeded) noexcept {
    if (generation == 0 || generation != state.generation ||
        state.owner != VoiceBufferOwner::audio ||
        (state.phase != VoiceLifecyclePhase::capturing &&
         state.phase != VoiceLifecyclePhase::cancelling_capture)) {
        return false;
    }
    state.owner = VoiceBufferOwner::transport;
    state.phase = succeeded && !state.cancel_requested
                      ? VoiceLifecyclePhase::upload_ready
                      : VoiceLifecyclePhase::cancelling_upload;
    return true;
}

bool voice_worker_should_upload(VoiceLifecycle &state,
                                std::uint32_t generation) noexcept {
    if (generation == 0 || generation != state.generation ||
        state.owner != VoiceBufferOwner::transport ||
        state.phase != VoiceLifecyclePhase::upload_ready ||
        state.cancel_requested) {
        return false;
    }
    state.phase = VoiceLifecyclePhase::uploading;
    return true;
}

bool voice_worker_released(VoiceLifecycle &state, std::uint32_t generation,
                           bool upload_succeeded) noexcept {
    if (generation == 0 || generation != state.generation ||
        state.owner != VoiceBufferOwner::transport ||
        (state.phase != VoiceLifecyclePhase::upload_ready &&
         state.phase != VoiceLifecyclePhase::uploading &&
         state.phase != VoiceLifecyclePhase::cancelling_upload)) {
        return false;
    }
    state.owner = VoiceBufferOwner::none;
    state.phase = upload_succeeded && !state.cancel_requested
                      ? VoiceLifecyclePhase::processing
                      : VoiceLifecyclePhase::terminal;
    return true;
}

bool voice_terminal_status_allowed(const VoiceLifecycle &state) noexcept {
    return state.phase == VoiceLifecyclePhase::processing;
}

bool voice_processing_completed(VoiceLifecycle &state) noexcept {
    if (state.phase != VoiceLifecyclePhase::processing ||
        state.owner != VoiceBufferOwner::none) {
        return false;
    }
    state.phase = VoiceLifecyclePhase::terminal;
    return true;
}

bool voice_is_quiescent(const VoiceLifecycle &state) noexcept {
    return state.owner == VoiceBufferOwner::none &&
           (state.phase == VoiceLifecyclePhase::idle ||
            state.phase == VoiceLifecyclePhase::terminal);
}

bool voice_note_progress(VoiceRetryWindow &window,
                         std::uint32_t acknowledged_offset,
                         std::uint32_t total_bytes) noexcept {
    if (acknowledged_offset <= window.acknowledged_offset ||
        acknowledged_offset > total_bytes) {
        return false;
    }
    window.acknowledged_offset = acknowledged_offset;
    window.failures = 0;
    return true;
}

bool voice_retry_after_timeout(VoiceRetryWindow &window,
                               std::uint8_t maximum_retries) noexcept {
    if (window.failures >= maximum_retries) return false;
    ++window.failures;
    return true;
}

bool voice_deadline_expired(std::int64_t now_us, std::int64_t deadline_us) noexcept {
    return deadline_us > 0 && now_us >= deadline_us;
}

}  // namespace nightglass::services
