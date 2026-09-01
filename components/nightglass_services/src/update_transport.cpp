#include "nightglass/services/update_transport.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nightglass/services/hardware.hpp"
#include "nightglass/services/voice.hpp"
#include "nightglass/update/service.hpp"

namespace nightglass::services {
namespace {

constexpr std::size_t kQueueDepth = 4;
UpdateTransport instance;
QueueHandle_t command_queue{};
TaskHandle_t worker_task{};
UpdateStatusSink status_sink{};
std::uint64_t active_session{};
nightglass::update::UpdateManifest active_manifest{};
portMUX_TYPE transport_mux = portMUX_INITIALIZER_UNLOCKED;
UpdateTransportSnapshot transport_snapshot{};
bool link_connected{};
std::int64_t link_lost_us{};
std::int64_t last_command_us{};

void wipe(void *memory, std::size_t length) noexcept {
    auto *bytes = static_cast<volatile std::uint8_t *>(memory);
    while (length--) *bytes++ = 0;
}

std::uint8_t result_code(const nightglass::core::Status &status) noexcept {
    return status.is_ok() ? 0 : static_cast<std::uint8_t>(status.code) + 1U;
}

bool same_manifest(const nightglass::update::UpdateManifest &left,
                   const nightglass::update::UpdateManifest &right) noexcept {
    return left.format_version == right.format_version &&
           left.partition_revision == right.partition_revision &&
           left.secure_version == right.secure_version &&
           left.image_size == right.image_size && left.board_id == right.board_id &&
           left.partition_id == right.partition_id &&
           left.app_version == right.app_version &&
           left.image_sha256 == right.image_sha256;
}

void publish_transport_snapshot(bool awaiting, bool ready_to_reboot = false) {
    portENTER_CRITICAL(&transport_mux);
    transport_snapshot.session = active_session;
    transport_snapshot.awaiting_confirmation = awaiting;
    transport_snapshot.ready_to_reboot = ready_to_reboot;
    transport_snapshot.target_version = active_manifest.app_version;
    ++transport_snapshot.sequence;
    portEXIT_CRITICAL(&transport_mux);
}

bool awaiting_confirmation() {
    portENTER_CRITICAL(&transport_mux);
    const bool value = transport_snapshot.awaiting_confirmation;
    portEXIT_CRITICAL(&transport_mux);
    return value;
}

void publish_status(std::uint64_t requested_session, std::uint8_t result) {
    auto &service = nightglass::update::update_service();
    const auto snapshot = service.snapshot();
    const UpdateTransportStatus status{
        .session = active_session != 0 ? active_session : requested_session,
        .state = static_cast<std::uint8_t>(snapshot.state),
        .signature_state = static_cast<std::uint8_t>(snapshot.signature_state),
        .result = result,
        .expected_bytes = snapshot.expected_bytes,
        .received_bytes = snapshot.received_bytes,
    };
    const auto frame = encode_update_transport_status(status);
    if (status_sink != nullptr) (void)status_sink(frame.data(), frame.size());
}

void handle(UpdateTransportCommand &command) {
    auto &service = nightglass::update::update_service();
    nightglass::core::Status result{};
    auto snapshot = service.snapshot();
    switch (command.kind) {
        case UpdateTransportCommandKind::begin:
            if (active_session != 0 && active_session != command.session) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "another update session is active"};
                break;
            }
            if (snapshot.state == nightglass::update::UpdateState::receiving) {
                if (active_session == command.session &&
                    same_manifest(active_manifest, command.manifest)) {
                    result = nightglass::core::Status::Ok();
                } else {
                    result = {nightglass::core::StatusCode::invalid_state,
                              "active update does not match resume request"};
                }
                break;
            }
            if (!voice_service().prepare_for_update(1500)) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "voice service did not become quiescent"};
                break;
            }
            if (snapshot.state == nightglass::update::UpdateState::failed) {
                const auto abort_result = service.abort();
                if (!abort_result.is_ok()) {
                    result = abort_result;
                    voice_service().update_finished();
                    break;
                }
            }
            {
            const auto battery = hardware_service().snapshot().battery;
            if (!battery.percent_valid || (!battery.charging && battery.percent < 40)) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "watch battery must be charging or at least 40 percent"};
                voice_service().update_finished();
                break;
            }
            result = service.begin_update(
                command.manifest,
                std::span(command.signature.data(), command.signature_size));
            if (result.is_ok()) {
                active_session = command.session;
                active_manifest = command.manifest;
                publish_transport_snapshot(false);
            } else {
                voice_service().update_finished();
            }
            }
            break;
        case UpdateTransportCommandKind::data:
            snapshot = service.snapshot();
            if (!update_stream_position_matches(
                    active_session, command.session, snapshot.received_bytes,
                    command.offset,
                    snapshot.state == nightglass::update::UpdateState::receiving,
                    awaiting_confirmation())) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update stream offset or session mismatch"};
            } else {
                result = service.write(std::span(command.data.data(), command.data_size));
            }
            break;
        case UpdateTransportCommandKind::finish:
            snapshot = service.snapshot();
            if (active_session != command.session ||
                snapshot.state != nightglass::update::UpdateState::receiving ||
                snapshot.received_bytes != snapshot.expected_bytes) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update is not complete"};
            } else {
                publish_transport_snapshot(true);
                result = nightglass::core::Status::Ok();
            }
            break;
        case UpdateTransportCommandKind::internal_confirm:
            if (active_session == 0 || command.session != active_session ||
                !awaiting_confirmation()) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update confirmation is not pending"};
            } else {
                result = service.finish();
                publish_transport_snapshot(false, result.is_ok());
            }
            break;
        case UpdateTransportCommandKind::abort:
            if (active_session != 0 && active_session != command.session) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update abort session mismatch"};
            } else {
                result = service.abort();
                if (result.is_ok()) {
                    active_session = 0;
                    active_manifest = {};
                    publish_transport_snapshot(false);
                    voice_service().update_finished();
                }
            }
            break;
        case UpdateTransportCommandKind::status:
            result = nightglass::core::Status::Ok();
            break;
        default:
            result = {nightglass::core::StatusCode::invalid_state,
                      "invalid update transport command"};
            break;
    }
    last_command_us = esp_timer_get_time();
    const auto code = result.is_ok() && awaiting_confirmation() ? std::uint8_t{9}
                                                                 : result_code(result);
    publish_status(command.session, code);
}

void worker(void *) {
    UpdateTransportCommand command{};
    while (true) {
        if (xQueueReceive(command_queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE) {
            handle(command);
            wipe(&command, sizeof(command));
        } else if (active_session != 0) {
            const auto now = esp_timer_get_time();
            portENTER_CRITICAL(&transport_mux);
            const bool disconnected_too_long = !link_connected && link_lost_us > 0 &&
                                               now - link_lost_us > 30'000'000;
            portEXIT_CRITICAL(&transport_mux);
            if (disconnected_too_long ||
                (last_command_us > 0 && now - last_command_us > 120'000'000)) {
                const auto abort_result = nightglass::update::update_service().abort();
                if (abort_result.is_ok()) {
                    active_session = 0;
                    active_manifest = {};
                    publish_transport_snapshot(false);
                    voice_service().update_finished();
                }
            }
        }
    }
}

}  // namespace

nightglass::core::Status UpdateTransport::start(UpdateStatusSink sink) {
    if (sink == nullptr) {
        return {nightglass::core::StatusCode::invalid_state,
                "update status sink unavailable"};
    }
    status_sink = sink;
    if (command_queue == nullptr) {
        command_queue = xQueueCreate(kQueueDepth, sizeof(UpdateTransportCommand));
        if (command_queue == nullptr) {
            return {nightglass::core::StatusCode::no_memory,
                    "update command queue unavailable"};
        }
    }
    if (worker_task == nullptr &&
        xTaskCreatePinnedToCore(worker, "nightglass_ota_rx", 6144, nullptr, 5,
                                &worker_task, 0) != pdPASS) {
        worker_task = nullptr;
        return {nightglass::core::StatusCode::no_memory,
                "update transport worker unavailable"};
    }
    return nightglass::core::Status::Ok();
}

bool UpdateTransport::enqueue(const UpdateTransportCommand &command) noexcept {
    return command_queue != nullptr &&
           xQueueSend(command_queue, &command, 0) == pdTRUE;
}

bool UpdateTransport::confirm() noexcept {
    UpdateTransportCommand command{};
    command.kind = UpdateTransportCommandKind::internal_confirm;
    portENTER_CRITICAL(&transport_mux);
    command.session = transport_snapshot.session;
    portEXIT_CRITICAL(&transport_mux);
    return command.session != 0 && enqueue(command);
}

bool UpdateTransport::abort() noexcept {
    UpdateTransportCommand command{};
    command.kind = UpdateTransportCommandKind::abort;
    portENTER_CRITICAL(&transport_mux);
    command.session = transport_snapshot.session;
    portEXIT_CRITICAL(&transport_mux);
    return command.session != 0 && enqueue(command);
}

bool UpdateTransport::restart() noexcept {
    bool ready = false;
    portENTER_CRITICAL(&transport_mux);
    ready = transport_snapshot.ready_to_reboot;
    portEXIT_CRITICAL(&transport_mux);
    if (!ready || nightglass::update::update_service().snapshot().state !=
                      nightglass::update::UpdateState::ready_to_reboot) {
        return false;
    }
    esp_restart();
    return true;
}

void UpdateTransport::link_ready() noexcept {
    portENTER_CRITICAL(&transport_mux);
    link_connected = true;
    link_lost_us = 0;
    portEXIT_CRITICAL(&transport_mux);
}

void UpdateTransport::link_lost() noexcept {
    portENTER_CRITICAL(&transport_mux);
    link_connected = false;
    link_lost_us = esp_timer_get_time();
    portEXIT_CRITICAL(&transport_mux);
}

UpdateTransportSnapshot UpdateTransport::snapshot() const noexcept {
    portENTER_CRITICAL(&transport_mux);
    const auto copy = transport_snapshot;
    portEXIT_CRITICAL(&transport_mux);
    return copy;
}

UpdateTransport &update_transport() { return instance; }

}  // namespace nightglass::services
