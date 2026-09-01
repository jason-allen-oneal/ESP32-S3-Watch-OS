#include <cassert>
#include <cstdint>

#include "nightglass/services/voice_state.hpp"

int main() {
    using namespace nightglass::services;

    VoiceLifecycle state{};
    assert(voice_can_begin(state));
    const auto first = voice_reserve_begin(state);
    assert(first != 0 && !voice_can_begin(state));
    assert(voice_capture_queued(state, first));
    assert(state.owner == VoiceBufferOwner::audio);

    // Cancel during capture retains audio ownership until the cleanup callback.
    assert(voice_request_cancel(state));
    assert(state.phase == VoiceLifecyclePhase::cancelling_capture);
    assert(!voice_can_begin(state));
    assert(voice_capture_completed(state, first, false));
    assert(state.owner == VoiceBufferOwner::transport);
    assert(!voice_can_begin(state));
    assert(!voice_worker_should_upload(state, first));
    assert(voice_worker_released(state, first, false));
    assert(voice_is_quiescent(state) && voice_can_begin(state));

    // OTA may proceed only after the active owner acknowledges shutdown.
    const auto ota_turn = voice_reserve_begin(state);
    assert(voice_capture_queued(state, ota_turn));
    assert(!voice_is_quiescent(state));
    assert(voice_request_cancel(state));
    assert(!voice_is_quiescent(state));
    assert(voice_capture_completed(state, ota_turn, false));
    assert(!voice_is_quiescent(state));
    assert(voice_worker_released(state, ota_turn, false));
    assert(voice_is_quiescent(state));

    // A stale callback cannot transfer or release the new turn's buffer.
    const auto second = voice_reserve_begin(state);
    assert(second != first && voice_capture_queued(state, second));
    assert(!voice_capture_completed(state, first, true));
    assert(state.owner == VoiceBufferOwner::audio);
    assert(voice_capture_completed(state, second, true));
    assert(voice_worker_should_upload(state, second));

    // Cancel during upload retains transport ownership until its worker exits.
    assert(voice_request_cancel(state));
    assert(state.phase == VoiceLifecyclePhase::cancelling_upload);
    assert(!voice_can_begin(state));
    assert(voice_worker_released(state, second, false));
    assert(voice_is_quiescent(state));

    // Disconnect uses the same cancellation path; immediate repress is rejected.
    const auto third = voice_reserve_begin(state);
    assert(voice_capture_queued(state, third));
    assert(voice_request_cancel(state));
    assert(!voice_can_begin(state));
    assert(voice_capture_completed(state, third, false));
    assert(voice_worker_released(state, third, false));

    // Processing accepts terminal status; capture/upload never do.
    const auto fourth = voice_reserve_begin(state);
    assert(voice_capture_queued(state, fourth));
    assert(!voice_terminal_status_allowed(state));
    assert(voice_capture_completed(state, fourth, true));
    assert(voice_worker_should_upload(state, fourth));
    assert(!voice_terminal_status_allowed(state));
    assert(voice_worker_released(state, fourth, true));
    assert(state.phase == VoiceLifecyclePhase::processing);
    assert(voice_terminal_status_allowed(state));
    assert(voice_processing_completed(state));
    assert(voice_is_quiescent(state));

    // Lost ACKs retry a bounded number of times and progress resets the budget.
    VoiceRetryWindow retry{};
    assert(voice_retry_after_timeout(retry, 2));
    assert(voice_retry_after_timeout(retry, 2));
    assert(!voice_retry_after_timeout(retry, 2));
    assert(voice_note_progress(retry, 232, 64000));
    assert(retry.failures == 0 && retry.acknowledged_offset == 232);
    assert(!voice_note_progress(retry, 232, 64000));
    assert(!voice_note_progress(retry, 64001, 64000));
    assert(voice_retry_after_timeout(retry, 2));

    assert(!voice_deadline_expired(99, 100));
    assert(voice_deadline_expired(100, 100));
    assert(!voice_deadline_expired(100, 0));

    VoiceLifecycle allocation_failure{};
    const auto failed_generation = voice_reserve_begin(allocation_failure);
    voice_begin_failed(allocation_failure, failed_generation + 1U);
    assert(!voice_is_quiescent(allocation_failure));
    voice_begin_failed(allocation_failure, failed_generation);
    assert(voice_is_quiescent(allocation_failure));
}
