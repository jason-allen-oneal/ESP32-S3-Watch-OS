#include "nightglass/services/voice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <span>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nightglass/services/audio.hpp"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/voice_state.hpp"
#include "nvs.h"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_voice";
constexpr char kVoiceNvsNamespace[] = "ng_voice";
constexpr char kVoiceSettingsKey[] = "settings";
constexpr std::uint32_t kAckTimeoutMs = 2500;
constexpr std::uint8_t kMaximumRetries = 2;
constexpr std::uint32_t kWorkerStackBytes = 4096;
constexpr std::uint32_t kUploadMinimumDeadlineMs = 60000;
constexpr std::uint32_t kUploadGraceMs = 30000;
constexpr std::uint32_t kConservativeUploadBytesPerSecond = 2048;
constexpr std::uint32_t kProcessingDeadlineMs = 90000;
constexpr std::uint32_t kWorkerPollMs = 250;
constexpr std::size_t kVoicePageBytes = 32U * 1024U;
constexpr std::size_t kVoiceMaximumPages =
    (kVoiceMaximumEncodedBytes + kVoicePageBytes - 1U) / kVoicePageBytes;

static_assert(kVoiceMaximumEncodedBytes == 2'400'000U);
static_assert(kVoiceMaximumPages == 74U);
static_assert(kVoiceMaximumEncodedBytes / (64U - kVoiceDataHeaderBytes) < 65535U,
              "voice sequence field must cover a 300-second turn at minimum MTU");

struct VoicePageStore {
    std::array<std::uint8_t *, kVoiceMaximumPages> pages{};
    std::size_t size{0};
    std::size_t limit{0};

    bool begin(std::size_t maximum_bytes) {
        if (size != 0 || limit != 0 || maximum_bytes == 0 ||
            maximum_bytes > kVoiceMaximumEncodedBytes) {
            return false;
        }
        limit = maximum_bytes;
        return true;
    }

    bool append(std::span<const std::uint8_t> encoded) {
        if (encoded.empty() || limit == 0 || size > limit ||
            encoded.size() > limit - size) return false;
        std::size_t source_offset = 0;
        while (source_offset < encoded.size()) {
            const auto page_index = size / kVoicePageBytes;
            const auto page_offset = size % kVoicePageBytes;
            if (page_index >= pages.size()) return false;
            if (pages[page_index] == nullptr) {
                pages[page_index] = static_cast<std::uint8_t *>(heap_caps_calloc(
                    kVoicePageBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (pages[page_index] == nullptr) return false;
            }
            const auto amount = std::min(encoded.size() - source_offset,
                                         kVoicePageBytes - page_offset);
            std::memcpy(pages[page_index] + page_offset,
                        encoded.data() + source_offset, amount);
            size += amount;
            source_offset += amount;
        }
        return true;
    }

    bool copy(std::size_t offset, std::span<std::uint8_t> destination) const {
        if (destination.empty() || offset > size || destination.size() > size - offset) {
            return false;
        }
        std::size_t destination_offset = 0;
        while (destination_offset < destination.size()) {
            const auto source_offset = offset + destination_offset;
            const auto page_index = source_offset / kVoicePageBytes;
            const auto page_offset = source_offset % kVoicePageBytes;
            if (page_index >= pages.size() || pages[page_index] == nullptr) return false;
            const auto amount = std::min(destination.size() - destination_offset,
                                         kVoicePageBytes - page_offset);
            std::memcpy(destination.data() + destination_offset,
                        pages[page_index] + page_offset, amount);
            destination_offset += amount;
        }
        return true;
    }
};

VoiceService instance;
portMUX_TYPE voice_lock = portMUX_INITIALIZER_UNLOCKED;
VoiceSnapshot current{};
VoicePageStore request_pages{};
TaskHandle_t voice_worker{nullptr};
std::atomic_bool cancel_requested{false};
std::uint32_t acknowledged_offset{};
std::uint8_t available_credits{};
VoiceStatus acknowledgement_status{VoiceStatus::ok};
std::uint32_t response_id{};
std::uint32_t response_crc{};
std::uint16_t response_expected{};
std::uint16_t response_received{};
std::int64_t recording_started_us{};
std::int64_t upload_deadline_us{};
std::int64_t processing_deadline_us{};
std::int64_t health_updated_us{};
VoiceLifecycle lifecycle{};
struct CaptureContext {
    std::uint32_t generation{0};
};
CaptureContext capture_context{};
std::atomic_bool update_blocked{false};
StaticSemaphore_t response_mutex_state{};
SemaphoreHandle_t response_mutex{nullptr};

void secure_wipe(void *memory, std::size_t length) {
    auto *bytes = static_cast<volatile std::uint8_t *>(memory);
    while (length--) *bytes++ = 0;
}

void wipe_response() {
    if (response_mutex == nullptr ||
        xSemaphoreTake(response_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    secure_wipe(current.response.data(), current.response.size());
    portENTER_CRITICAL(&voice_lock);
    current.response_bytes = 0;
    response_id = 0;
    response_expected = 0;
    response_received = 0;
    response_crc = 0;
    portEXIT_CRITICAL(&voice_lock);
    xSemaphoreGive(response_mutex);
}

void release_request_buffer() {
    for (auto *&page : request_pages.pages) {
        if (page == nullptr) continue;
        secure_wipe(page, kVoicePageBytes);
        heap_caps_free(page);
        page = nullptr;
    }
    request_pages.size = 0;
    request_pages.limit = 0;
}

bool append_capture_page(void *, std::span<const std::uint8_t> encoded) {
    return request_pages.append(encoded);
}

std::uint32_t request_crc32() {
    std::uint32_t crc = 0xffffffffU;
    std::size_t offset = 0;
    while (offset < request_pages.size) {
        const auto page_index = offset / kVoicePageBytes;
        if (page_index >= request_pages.pages.size() ||
            request_pages.pages[page_index] == nullptr) return 0;
        const auto amount = std::min(kVoicePageBytes, request_pages.size - offset);
        const auto bytes = std::span<const std::uint8_t>(
            request_pages.pages[page_index], amount);
        for (const auto byte : bytes) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit) {
                const auto mask = static_cast<std::uint32_t>(
                    -static_cast<std::int32_t>(crc & 1U));
                crc = (crc >> 1U) ^ (0xedb88320U & mask);
            }
        }
        offset += amount;
    }
    return ~crc;
}

VoiceSettings load_voice_settings() {
    VoiceSettings settings{};
    nvs_handle_t handle{};
    if (nvs_open(kVoiceNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return settings;
    std::size_t size = sizeof(settings);
    VoiceSettings candidate{};
    const auto status = nvs_get_blob(handle, kVoiceSettingsKey, &candidate, &size);
    nvs_close(handle);
    return status == ESP_OK && size == sizeof(candidate) && valid_voice_settings(candidate)
               ? candidate
               : settings;
}

esp_err_t save_voice_settings(const VoiceSettings &settings) {
    nvs_handle_t handle{};
    auto status = nvs_open(kVoiceNvsNamespace, NVS_READWRITE, &handle);
    if (status == ESP_OK) status = nvs_set_blob(handle, kVoiceSettingsKey,
                                                &settings, sizeof(settings));
    if (status == ESP_OK) status = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return status;
}

std::uint32_t upload_deadline_ms(std::size_t bytes) {
    const auto transfer_ms = static_cast<std::uint64_t>(bytes) * 1000U /
                             kConservativeUploadBytesPerSecond;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::max<std::uint64_t>(kUploadMinimumDeadlineMs,
                                transfer_ms + kUploadGraceMs),
        std::numeric_limits<std::uint32_t>::max()));
}

void publish_failure(VoiceStatus status) {
    wipe_response();
    portENTER_CRITICAL(&voice_lock);
    current.state = status == VoiceStatus::cancelled
                        ? VoiceTurnState::cancelled
                        : VoiceTurnState::failed;
    current.status = status;
    current.recorded_ms = 0;
    ++current.sequence;
    portEXIT_CRITICAL(&voice_lock);
    ESP_LOGW(kTag, "Voice state failed: status=%u",
             static_cast<unsigned>(status));
}

bool upload_expired() {
    return voice_deadline_expired(esp_timer_get_time(), upload_deadline_us);
}

bool wait_for_notification() {
    if (cancel_requested.load(std::memory_order_acquire) || upload_expired()) return false;
    const auto remaining_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(
        1, (upload_deadline_us - esp_timer_get_time()) / 1000));
    const auto wait_ms = std::min(kAckTimeoutMs, remaining_ms);
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    return !cancel_requested.load(std::memory_order_acquire) && !upload_expired();
}

bool upload_request() {
    const auto session = current.session_id;
    const auto total = static_cast<std::uint32_t>(request_pages.size);
    if (session == 0 || total == 0 || total > kVoiceMaximumEncodedBytes) return false;
    const auto crc = request_crc32();
    auto begin = encode_voice_begin(session, total, crc);
    if (begin.size == 0) return false;
    VoiceRetryWindow begin_retry{};
    bool begin_accepted = false;
    do {
        if (!connectivity_service().send_voice_frame({begin.bytes.data(), begin.size}) ||
            !wait_for_notification()) {
            if (cancel_requested.load(std::memory_order_acquire) || upload_expired()) return false;
        }
        portENTER_CRITICAL(&voice_lock);
        begin_accepted = acknowledgement_status == VoiceStatus::ok &&
                         acknowledged_offset == 0 && available_credits > 0;
        const auto begin_status = acknowledgement_status;
        portEXIT_CRITICAL(&voice_lock);
        if (begin_status != VoiceStatus::ok) return false;
        if (begin_accepted) break;
    } while (voice_retry_after_timeout(begin_retry, kMaximumRetries));
    if (!begin_accepted) return false;

    std::uint32_t offset = 0;
    const auto frame_limit = connectivity_service().maximum_outbound_frame();
    if (frame_limit <= kVoiceDataHeaderBytes) return false;
    const auto data_payload_limit = std::min<std::size_t>(
        kVoiceMaximumDataPayloadBytes, frame_limit - kVoiceDataHeaderBytes);
    if (data_payload_limit == 0 ||
        (total + data_payload_limit - 1U) / data_payload_limit > 0xffffU) {
        return false;
    }
    std::array<std::uint8_t, kVoiceMaximumDataPayloadBytes> payload{};
    VoiceRetryWindow retry{};
    while (offset < total) {
        if (cancel_requested.load(std::memory_order_acquire)) return false;
        portENTER_CRITICAL(&voice_lock);
        auto credits = available_credits;
        portEXIT_CRITICAL(&voice_lock);
        if (credits == 0 || upload_expired()) return false;
        const auto window_start = offset;
        auto send_offset = offset;
        bool sent_any = false;
        bool send_failed = false;
        while (send_offset < total && credits-- > 0) {
            const auto payload_size = std::min<std::size_t>(
                data_payload_limit, static_cast<std::size_t>(total - send_offset));
            const auto sequence = static_cast<std::uint16_t>(
                send_offset / data_payload_limit + 1U);
            auto payload_view = std::span<std::uint8_t>(payload.data(), payload_size);
            if (!request_pages.copy(send_offset, payload_view)) {
                secure_wipe(payload.data(), payload.size());
                return false;
            }
            const auto frame = encode_voice_data(
                session, sequence, send_offset,
                payload_view);
            secure_wipe(payload.data(), payload_size);
            if (frame.size == 0 || !connectivity_service().send_voice_frame(
                                       {frame.bytes.data(), frame.size})) {
                send_failed = true;
                break;
            }
            sent_any = true;
            send_offset += static_cast<std::uint32_t>(payload_size);
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        if (sent_any && !wait_for_notification() &&
            (cancel_requested.load(std::memory_order_acquire) || upload_expired())) {
            return false;
        }
        portENTER_CRITICAL(&voice_lock);
        const auto ack = acknowledged_offset;
        const auto status = acknowledgement_status;
        portEXIT_CRITICAL(&voice_lock);
        if (status != VoiceStatus::ok || ack > send_offset || ack > total) return false;
        if (voice_note_progress(retry, ack, total)) {
            offset = retry.acknowledged_offset;
        } else if (!voice_retry_after_timeout(retry, kMaximumRetries)) {
            return false;
        } else {
            offset = window_start;
        }
        if (send_failed && !sent_any && upload_expired()) return false;
    }

    const auto end = encode_voice_end(session, total, crc);
    if (end.size == 0) return false;
    VoiceRetryWindow end_retry{.acknowledged_offset = total - 1U};
    bool end_accepted = false;
    do {
        if (!connectivity_service().send_voice_frame({end.bytes.data(), end.size}) ||
            !wait_for_notification()) {
            if (cancel_requested.load(std::memory_order_acquire) || upload_expired()) return false;
        }
        portENTER_CRITICAL(&voice_lock);
        const auto ack = acknowledged_offset;
        const auto status = acknowledgement_status;
        portEXIT_CRITICAL(&voice_lock);
        if (status != VoiceStatus::ok) return false;
        end_accepted = ack == total;
        if (end_accepted) break;
    } while (voice_retry_after_timeout(end_retry, kMaximumRetries));
    if (!end_accepted) return false;
    portENTER_CRITICAL(&voice_lock);
    const bool complete = acknowledged_offset == total &&
                          acknowledgement_status == VoiceStatus::ok;
    portEXIT_CRITICAL(&voice_lock);
    secure_wipe(payload.data(), payload.size());
    return complete;
}

void voice_worker_task(void *) {
    while (true) {
        portENTER_CRITICAL(&voice_lock);
        const bool processing = lifecycle.phase == VoiceLifecyclePhase::processing;
        portEXIT_CRITICAL(&voice_lock);
        (void)ulTaskNotifyTake(pdTRUE, processing ? pdMS_TO_TICKS(kWorkerPollMs)
                                                 : portMAX_DELAY);
        portENTER_CRITICAL(&voice_lock);
        const auto active_generation = lifecycle.generation;
        const bool upload = voice_worker_should_upload(lifecycle, active_generation);
        const bool release = lifecycle.owner == VoiceBufferOwner::transport;
        const bool processing_expired = lifecycle.phase == VoiceLifecyclePhase::processing &&
            voice_deadline_expired(esp_timer_get_time(), processing_deadline_us);
        portEXIT_CRITICAL(&voice_lock);
        if (processing_expired) {
            portENTER_CRITICAL(&voice_lock);
            (void)voice_request_cancel(lifecycle);
            portEXIT_CRITICAL(&voice_lock);
            publish_failure(VoiceStatus::timeout);
            continue;
        }
        if (!release) continue;
        const bool success = upload && upload_request();
        release_request_buffer();
        portENTER_CRITICAL(&voice_lock);
        const bool transitioned = voice_worker_released(
            lifecycle, active_generation, upload && success);
        const bool cancelled = lifecycle.cancel_requested;
        if (transitioned && upload && success) {
            current.state = VoiceTurnState::processing;
            current.status = VoiceStatus::ok;
            processing_deadline_us = esp_timer_get_time() +
                                     static_cast<std::int64_t>(kProcessingDeadlineMs) * 1000;
            ++current.sequence;
        }
        portEXIT_CRITICAL(&voice_lock);
        if (!transitioned) {
            publish_failure(VoiceStatus::processing_failed);
        } else if (!upload || !success) {
            const auto session = current.session_id;
            if (session != 0) {
                const auto cancel = encode_voice_cancel(
                    session, cancelled
                                 ? VoiceStatus::cancelled
                                 : VoiceStatus::timeout);
                if (cancel.size != 0) {
                    (void)connectivity_service().send_voice_frame(
                        {cancel.bytes.data(), cancel.size});
                }
            }
            publish_failure(cancelled ? VoiceStatus::cancelled : VoiceStatus::timeout);
        }
    }
}

void capture_complete(void *context, const VoiceCaptureResult &result) {
    const auto generation = static_cast<CaptureContext *>(context)->generation;
    const bool stored_complete = result.encoded_bytes == request_pages.size &&
                                 result.encoded_bytes <= request_pages.limit;
    portENTER_CRITICAL(&voice_lock);
    const bool lifecycle_accepted = voice_capture_completed(
        lifecycle, generation,
        !result.cancelled && result.status == nightglass::core::StatusCode::ok &&
            result.encoded_bytes > 0 && stored_complete);
    if (!lifecycle_accepted) {
        portEXIT_CRITICAL(&voice_lock);
        return;
    }
    current.capture_rms = result.rms;
    current.encoded_bytes = static_cast<std::uint32_t>(result.encoded_bytes);
    current.recorded_ms = static_cast<std::uint32_t>(
        result.encoded_bytes * 1000U / kVoiceTransportSampleRateHz);
    const bool cancelled = result.cancelled ||
                           cancel_requested.load(std::memory_order_acquire);
    const bool ready = !cancelled && result.status == nightglass::core::StatusCode::ok &&
                       result.encoded_bytes > 0 && stored_complete;
    current.state = ready ? VoiceTurnState::uploading : VoiceTurnState::finishing;
    current.status = ready ? VoiceStatus::ok
                           : cancelled ? VoiceStatus::cancelled
                                       : VoiceStatus::processing_failed;
    ++current.sequence;
    upload_deadline_us = esp_timer_get_time() +
                         static_cast<std::int64_t>(
                             upload_deadline_ms(result.encoded_bytes)) * 1000;
    const auto logged_session = current.session_id;
    const auto logged_ms = current.recorded_ms;
    const auto logged_bytes = current.encoded_bytes;
    const auto logged_rms = current.capture_rms;
    portEXIT_CRITICAL(&voice_lock);
    ESP_LOGI(kTag, "Voice capture closed: session=%lu ms=%lu bytes=%lu rms=%lu ready=%u",
             static_cast<unsigned long>(logged_session),
             static_cast<unsigned long>(logged_ms),
             static_cast<unsigned long>(logged_bytes),
             static_cast<unsigned long>(logged_rms),
             ready ? 1U : 0U);
    if (voice_worker != nullptr) xTaskNotifyGive(voice_worker);
}

}  // namespace

nightglass::core::Status VoiceService::start() {
    current = {};
    current.state = VoiceTurnState::idle;
    current.health = VoiceHealthState::unavailable;
    current.health_sequence = 0;
    health_updated_us = 0;
    current.settings = load_voice_settings();
    lifecycle = {};
    update_blocked.store(false, std::memory_order_release);
    response_mutex = xSemaphoreCreateMutexStatic(&response_mutex_state);
    if (response_mutex == nullptr) {
        current.state = VoiceTurnState::unavailable;
        return {nightglass::core::StatusCode::no_memory,
                "voice response mutex unavailable"};
    }
    if (xTaskCreateWithCaps(voice_worker_task, "ng_voice", kWorkerStackBytes,
                            nullptr, 3, &voice_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        voice_worker = nullptr;
        current.state = VoiceTurnState::unavailable;
        return {nightglass::core::StatusCode::no_memory,
                "voice transport worker unavailable"};
    }
    return nightglass::core::Status::Ok();
}

nightglass::core::Status VoiceService::begin_capture() {
    const auto link = connectivity_service().snapshot();
    if (link.state != CompanionLinkState::connected_encrypted ||
        !link.encrypted || !link.bonded || !link.peer_identity_pinned ||
        connectivity_service().maximum_outbound_frame() < 64) {
        publish_failure(VoiceStatus::disconnected);
        return {nightglass::core::StatusCode::unavailable,
                "secure phone link unavailable"};
    }
    portENTER_CRITICAL(&voice_lock);
    const bool update_active = update_blocked.load(std::memory_order_acquire);
    const auto generation = update_active ? 0U : voice_reserve_begin(lifecycle);
    portEXIT_CRITICAL(&voice_lock);
    if (generation == 0) {
        // Preserve a turn already recording/uploading/processing. Repeated
        // press events must not replace the real turn state with a synthetic
        // busy error. An OTA block is quiescent and should remain visible.
        if (update_active) publish_failure(VoiceStatus::busy);
        return update_active
                   ? nightglass::core::Status{nightglass::core::StatusCode::unavailable,
                                              "firmware update is active"}
                   : nightglass::core::Status{nightglass::core::StatusCode::invalid_state,
                                              "voice turn is not quiescent"};
    }

    if (request_pages.size != 0 || request_pages.limit != 0) {
        portENTER_CRITICAL(&voice_lock);
        voice_begin_failed(lifecycle, generation);
        portEXIT_CRITICAL(&voice_lock);
        publish_failure(VoiceStatus::processing_failed);
        return {nightglass::core::StatusCode::invalid_state,
                "voice buffer ownership unresolved"};
    }
    VoiceSettings settings{};
    portENTER_CRITICAL(&voice_lock);
    settings = current.settings;
    portEXIT_CRITICAL(&voice_lock);
    const auto request_capacity = voice_encoded_capacity(
        settings.maximum_duration_seconds);
    if (!request_pages.begin(request_capacity)) {
        portENTER_CRITICAL(&voice_lock);
        voice_begin_failed(lifecycle, generation);
        portEXIT_CRITICAL(&voice_lock);
        publish_failure(VoiceStatus::processing_failed);
        return {nightglass::core::StatusCode::no_memory,
                "voice page store unavailable"};
    }
    std::uint32_t session{};
    while (session == 0) session = esp_random();
    cancel_requested.store(false, std::memory_order_release);
    wipe_response();
    portENTER_CRITICAL(&voice_lock);
    const auto next_sequence = current.sequence + 1U;
    current.state = VoiceTurnState::recording;
    current.session_id = session;
    current.recorded_ms = 0;
    current.encoded_bytes = 0;
    current.capture_rms = 0;
    current.response_bytes = 0;
    current.status = VoiceStatus::ok;
    current.sequence = next_sequence == 0 ? 1 : next_sequence;
    acknowledged_offset = 0;
    available_credits = 0;
    acknowledgement_status = VoiceStatus::ok;
    response_id = 0;
    response_expected = 0;
    response_received = 0;
    response_crc = 0;
    recording_started_us = esp_timer_get_time();
    capture_context.generation = generation;
    const bool capture_reserved = voice_capture_queued(lifecycle, generation);
    portEXIT_CRITICAL(&voice_lock);
    if (!capture_reserved) {
        release_request_buffer();
        publish_failure(VoiceStatus::processing_failed);
        return {nightglass::core::StatusCode::invalid_state,
                "voice capture ownership unavailable"};
    }
    ESP_LOGI(kTag, "Voice capture started: session=%lu limit_s=%u",
             static_cast<unsigned long>(session),
             static_cast<unsigned>(settings.maximum_duration_seconds));
    const auto status = audio_service().request_voice_capture(
        request_capacity, append_capture_page, capture_complete, &capture_context);
    if (!status.is_ok()) {
        portENTER_CRITICAL(&voice_lock);
        lifecycle.owner = VoiceBufferOwner::none;
        lifecycle.phase = VoiceLifecyclePhase::terminal;
        portEXIT_CRITICAL(&voice_lock);
        release_request_buffer();
        publish_failure(VoiceStatus::busy);
        return status;
    }
    return nightglass::core::Status::Ok();
}

void VoiceService::finish_capture() {
    portENTER_CRITICAL(&voice_lock);
    const bool active = current.state == VoiceTurnState::recording;
    if (active) {
        current.state = VoiceTurnState::finishing;
        ++current.sequence;
    }
    portEXIT_CRITICAL(&voice_lock);
    if (active) audio_service().stop_voice_capture(false);
}

void VoiceService::cancel() {
    cancel_requested.store(true, std::memory_order_release);
    portENTER_CRITICAL(&voice_lock);
    const auto session = current.session_id;
    const auto owner = lifecycle.owner;
    const bool active = voice_request_cancel(lifecycle);
    if (active) {
        current.state = owner == VoiceBufferOwner::none
                            ? VoiceTurnState::cancelled
                            : VoiceTurnState::finishing;
        current.status = VoiceStatus::cancelled;
        ++current.sequence;
    }
    portEXIT_CRITICAL(&voice_lock);
    if (owner == VoiceBufferOwner::audio) audio_service().stop_voice_capture(true);
    if (active && session != 0) {
        const auto frame = encode_voice_cancel(session, VoiceStatus::cancelled);
        if (frame.size != 0) {
            (void)connectivity_service().send_voice_frame(
                {frame.bytes.data(), frame.size});
        }
    }
    if (voice_worker != nullptr && owner != VoiceBufferOwner::audio) xTaskNotifyGive(voice_worker);
    wipe_response();
}

void VoiceService::link_lost() {
    cancel();
    portENTER_CRITICAL(&voice_lock);
    current.health = VoiceHealthState::unavailable;
    current.health_sequence = 0;
    health_updated_us = 0;
    ++current.sequence;
    portEXIT_CRITICAL(&voice_lock);
    ESP_LOGW(kTag, "OpenClaw health unavailable: phone link lost");
}

bool VoiceService::prepare_for_update(std::uint32_t timeout_ms) {
    update_blocked.store(true, std::memory_order_release);
    cancel();
    const auto deadline = esp_timer_get_time() +
                          static_cast<std::int64_t>(timeout_ms) * 1000;
    while (!quiescent() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (quiescent()) return true;
    update_blocked.store(false, std::memory_order_release);
    return false;
}

void VoiceService::update_finished() {
    update_blocked.store(false, std::memory_order_release);
}

bool VoiceService::quiescent() const {
    portENTER_CRITICAL(&voice_lock);
    const bool value = voice_is_quiescent(lifecycle) && request_pages.size == 0 &&
                       request_pages.limit == 0;
    portEXIT_CRITICAL(&voice_lock);
    return value;
}

nightglass::core::Status VoiceService::update_settings(
    const VoiceSettings &settings) {
    if (!valid_voice_settings(settings)) {
        return {nightglass::core::StatusCode::invalid_state,
                "invalid voice duration"};
    }
    portENTER_CRITICAL(&voice_lock);
    const bool busy = !voice_is_quiescent(lifecycle);
    portEXIT_CRITICAL(&voice_lock);
    if (busy) {
        return {nightglass::core::StatusCode::unavailable,
                "finish the current voice turn first"};
    }
    if (save_voice_settings(settings) != ESP_OK) {
        return {nightglass::core::StatusCode::io_error,
                "voice settings could not be saved"};
    }
    portENTER_CRITICAL(&voice_lock);
    current.settings = settings;
    ++current.sequence;
    portEXIT_CRITICAL(&voice_lock);
    return nightglass::core::Status::Ok();
}

bool VoiceService::accept_frame(const VoiceFrame &frame) {
    if (frame.kind == VoiceFrameKind::health) {
        portENTER_CRITICAL(&voice_lock);
        const bool newer = voice_health_sequence_is_newer(
            frame.session_id, current.health_sequence);
        if (newer) {
            current.health_sequence = frame.session_id;
            current.health = frame.health;
            health_updated_us = esp_timer_get_time();
            ++current.sequence;
        }
        portEXIT_CRITICAL(&voice_lock);
        if (newer) {
            ESP_LOGI(kTag, "OpenClaw health updated: state=%u sequence=%lu",
                     static_cast<unsigned>(frame.health),
                     static_cast<unsigned long>(frame.session_id));
        }
        return newer;
    }
    const bool response_payload = frame.kind == VoiceFrameKind::response_begin ||
                                  frame.kind == VoiceFrameKind::response_data ||
                                  frame.kind == VoiceFrameKind::response_end;
    if (response_payload &&
        (response_mutex == nullptr ||
         xSemaphoreTake(response_mutex, pdMS_TO_TICKS(10)) != pdTRUE)) {
        return false;
    }
    portENTER_CRITICAL(&voice_lock);
    if (frame.session_id == 0 || frame.session_id != current.session_id) {
        portEXIT_CRITICAL(&voice_lock);
        if (response_payload) xSemaphoreGive(response_mutex);
        return false;
    }
    bool accepted = false;
    bool clear_response = false;
    if (frame.kind == VoiceFrameKind::request_ack &&
        (current.state == VoiceTurnState::uploading ||
         current.state == VoiceTurnState::processing) &&
        frame.offset >= acknowledged_offset &&
        frame.offset <= current.encoded_bytes) {
        acknowledged_offset = frame.offset;
        available_credits = frame.credits;
        acknowledgement_status = frame.status;
        accepted = true;
    } else if (frame.kind == VoiceFrameKind::response_begin &&
               current.state == VoiceTurnState::processing && response_id == 0) {
        response_id = frame.response_id;
        response_expected = static_cast<std::uint16_t>(frame.total_bytes);
        response_received = 0;
        response_crc = frame.crc32;
        current.response_bytes = 0;
        clear_response = true;
        accepted = true;
    } else if (frame.kind == VoiceFrameKind::response_data &&
               current.state == VoiceTurnState::processing &&
               frame.response_id == response_id && frame.offset == response_received &&
               std::all_of(frame.payload.begin(), frame.payload.end(), [](std::uint8_t byte) {
                   return byte == '\n' || (byte >= 0x20 && byte <= 0x7e);
               }) &&
               frame.payload.size() <= response_expected - response_received) {
        std::copy(frame.payload.begin(), frame.payload.end(),
                  reinterpret_cast<std::uint8_t *>(current.response.data()) +
                      response_received);
        response_received = static_cast<std::uint16_t>(
            response_received + frame.payload.size());
        accepted = true;
    } else if (frame.kind == VoiceFrameKind::response_end &&
               current.state == VoiceTurnState::processing &&
               frame.response_id == response_id && frame.total_bytes == response_expected &&
               response_received == response_expected && frame.crc32 == response_crc) {
        const auto bytes = response_received;
        const auto expected_crc = response_crc;
        portEXIT_CRITICAL(&voice_lock);
        const bool crc_valid = voice_crc32({
            reinterpret_cast<const std::uint8_t *>(current.response.data()), bytes}) ==
            expected_crc;
        portENTER_CRITICAL(&voice_lock);
        const bool still_current = current.state == VoiceTurnState::processing &&
                                   frame.response_id == response_id &&
                                   response_received == bytes &&
                                   response_crc == expected_crc;
        if (crc_valid && still_current && voice_processing_completed(lifecycle)) {
            current.response[bytes] = '\0';
            current.response_bytes = bytes;
            current.state = VoiceTurnState::complete;
            current.status = VoiceStatus::ok;
            ++current.sequence;
            accepted = true;
        }
    } else if (frame.kind == VoiceFrameKind::response_status &&
               frame.status != VoiceStatus::ok &&
               voice_terminal_status_allowed(lifecycle)) {
        (void)voice_processing_completed(lifecycle);
        current.state = frame.status == VoiceStatus::cancelled
                            ? VoiceTurnState::cancelled
                            : VoiceTurnState::failed;
        current.status = frame.status;
        current.response_bytes = 0;
        response_id = 0;
        response_expected = 0;
        response_received = 0;
        response_crc = 0;
        clear_response = true;
        ++current.sequence;
        accepted = true;
    }
    portEXIT_CRITICAL(&voice_lock);
    if (clear_response) secure_wipe(current.response.data(), current.response.size());
    if (response_payload) xSemaphoreGive(response_mutex);
    if (accepted && frame.kind == VoiceFrameKind::response_end) {
        ESP_LOGI(kTag, "Voice response complete: session=%lu bytes=%lu",
                 static_cast<unsigned long>(frame.session_id),
                 static_cast<unsigned long>(frame.total_bytes));
    } else if (accepted && frame.kind == VoiceFrameKind::response_status) {
        ESP_LOGW(kTag, "Voice response failed: session=%lu status=%u",
                 static_cast<unsigned long>(frame.session_id),
                 static_cast<unsigned>(frame.status));
    }
    if (accepted && voice_worker != nullptr) xTaskNotifyGive(voice_worker);
    return accepted;
}

VoiceSnapshot VoiceService::snapshot() const {
    VoiceSnapshot snapshot{};
    std::int64_t sampled_health_updated_us{};
    portENTER_CRITICAL(&voice_lock);
    snapshot.sequence = current.sequence;
    snapshot.state = current.state;
    snapshot.session_id = current.session_id;
    snapshot.recorded_ms = current.recorded_ms;
    snapshot.encoded_bytes = current.encoded_bytes;
    snapshot.capture_rms = current.capture_rms;
    snapshot.response_bytes = current.response_bytes;
    snapshot.status = current.status;
    snapshot.health = current.health;
    snapshot.health_sequence = current.health_sequence;
    sampled_health_updated_us = health_updated_us;
    snapshot.settings = current.settings;
    if (snapshot.state == VoiceTurnState::recording && recording_started_us > 0) {
        const auto elapsed = esp_timer_get_time() - recording_started_us;
        snapshot.recorded_ms = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(elapsed / 1000, 0,
                                    kVoiceMaximumDurationSeconds * 1000));
    }
    portEXIT_CRITICAL(&voice_lock);
    const auto link = connectivity_service().snapshot();
    const auto now_us = esp_timer_get_time();
    if (link.state != CompanionLinkState::connected_encrypted || !link.encrypted ||
        !link.bonded || !link.peer_identity_pinned) {
        snapshot.health = VoiceHealthState::unavailable;
        snapshot.health_age_seconds = 0;
    } else if (snapshot.health_sequence == 0 ||
               voice_health_is_stale(now_us, sampled_health_updated_us)) {
        snapshot.health = VoiceHealthState::degraded;
        snapshot.health_age_seconds = sampled_health_updated_us > 0 &&
                                              now_us >= sampled_health_updated_us
                                          ? static_cast<std::uint32_t>(
                                                (now_us - sampled_health_updated_us) / 1'000'000)
                                          : 0;
    } else {
        snapshot.health_age_seconds = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, now_us - sampled_health_updated_us) / 1'000'000);
    }
    if (snapshot.response_bytes > 0 && response_mutex != nullptr &&
        xSemaphoreTake(response_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        std::memcpy(snapshot.response.data(), current.response.data(),
                    snapshot.response_bytes + 1U);
        xSemaphoreGive(response_mutex);
    } else {
        snapshot.response_bytes = 0;
    }
    return snapshot;
}

VoiceService &voice_service() { return instance; }

}  // namespace nightglass::services
