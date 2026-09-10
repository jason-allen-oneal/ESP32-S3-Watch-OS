#include "nightglass/services/update_transport.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
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
std::array<UpdateStatusSink, 3> status_sinks{};
std::array<std::uint32_t, 3> link_epochs{};
std::uint64_t active_session{};
UpdateTransportLink active_link{UpdateTransportLink::none};
nightglass::update::UpdateManifest active_manifest{};
portMUX_TYPE transport_mux = portMUX_INITIALIZER_UNLOCKED;
UpdateTransportSnapshot transport_snapshot{};
std::array<bool, 3> link_connected{};
std::int64_t link_lost_us{};
std::int64_t last_command_us{};

void wipe(void *memory, std::size_t length) noexcept {
    auto *bytes = static_cast<volatile std::uint8_t *>(memory);
    while (length--) *bytes++ = 0;
}

std::size_t link_index(UpdateTransportLink link) noexcept {
    return static_cast<std::size_t>(link);
}

bool valid_link(UpdateTransportLink link) noexcept {
    return link == UpdateTransportLink::ble || link == UpdateTransportLink::usb;
}

std::uint8_t command_opcode(UpdateTransportCommandKind kind) noexcept {
    switch (kind) {
        case UpdateTransportCommandKind::begin:
            return kUpdateBeginOpcode;
        case UpdateTransportCommandKind::data:
            return kUpdateDataOpcode;
        case UpdateTransportCommandKind::finish:
            return kUpdateFinishOpcode;
        case UpdateTransportCommandKind::abort:
            return kUpdateAbortOpcode;
        case UpdateTransportCommandKind::status:
            return kUpdateStatusQueryOpcode;
        default:
            return 0;
    }
}

bool connection_epoch_current(const UpdateTransportCommand &command) noexcept {
    if (!valid_link(command.link) || command.connection_epoch == 0) return false;
    portENTER_CRITICAL(&transport_mux);
    const bool current_epoch =
        link_epochs[link_index(command.link)] == command.connection_epoch;
    portEXIT_CRITICAL(&transport_mux);
    return current_epoch;
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
    transport_snapshot.link = active_link;
    transport_snapshot.awaiting_confirmation = awaiting;
    transport_snapshot.ready_to_reboot = ready_to_reboot;
    transport_snapshot.target_version = active_manifest.app_version;
    ++transport_snapshot.sequence;
    portEXIT_CRITICAL(&transport_mux);
}

void publish_status(UpdateTransportLink requested_link, std::uint64_t requested_session,
                    std::uint8_t result, UpdateTransportCommandKind kind) {
    auto &service = nightglass::update::update_service();
    const auto snapshot = service.snapshot();
    const UpdateTransportStatus status{
        .session = active_session != 0 ? active_session : requested_session,
        .state = static_cast<std::uint8_t>(snapshot.state),
        .signature_state = static_cast<std::uint8_t>(snapshot.signature_state),
        .result = result,
        // Preserve the deployed BLE status layout for older companion builds.
        // USB uses this byte to correlate pipelined acknowledgements.
        .acknowledged_opcode = requested_link == UpdateTransportLink::usb
                                   ? command_opcode(kind)
                                   : std::uint8_t{0},
        .expected_bytes = snapshot.expected_bytes,
        .received_bytes = snapshot.received_bytes,
    };
    const auto frame = encode_update_transport_status(status);
    UpdateStatusSink sink = nullptr;
    if (valid_link(requested_link)) {
        portENTER_CRITICAL(&transport_mux);
        sink = status_sinks[link_index(requested_link)];
        portEXIT_CRITICAL(&transport_mux);
    }
    if (sink != nullptr) (void)sink(frame.data(), frame.size());
}

void handle(UpdateTransportCommand &command) {
    auto &service = nightglass::update::update_service();
    nightglass::core::Status result{};
    bool restart_after_status = false;
    auto snapshot = service.snapshot();
    switch (command.kind) {
        case UpdateTransportCommandKind::begin:
            if (active_session != 0 &&
                (active_session != command.session || active_link != command.link)) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "another update session is active"};
                break;
            }
            if (snapshot.state == nightglass::update::UpdateState::receiving) {
                if (active_session == command.session && active_link == command.link &&
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
            const bool usb_powered = command.link == UpdateTransportLink::usb &&
                                     usb_serial_jtag_is_connected();
            if (!usb_powered &&
                (!battery.percent_valid || (!battery.charging && battery.percent < 40))) {
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
                active_link = command.link;
                active_manifest = command.manifest;
                portENTER_CRITICAL(&transport_mux);
                link_lost_us = link_connected[link_index(active_link)]
                                   ? 0
                                   : esp_timer_get_time();
                portEXIT_CRITICAL(&transport_mux);
                publish_transport_snapshot(false);
            } else {
                voice_service().update_finished();
            }
            }
            break;
        case UpdateTransportCommandKind::data:
            snapshot = service.snapshot();
            if (active_link != command.link || !update_stream_position_matches(
                    active_session, command.session, snapshot.received_bytes,
                    command.offset,
                    snapshot.state == nightglass::update::UpdateState::receiving,
                    false)) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update stream offset or session mismatch"};
            } else {
                result = service.write(std::span(command.data.data(), command.data_size));
            }
            break;
        case UpdateTransportCommandKind::finish:
            snapshot = service.snapshot();
            if (active_session != command.session || active_link != command.link ||
                snapshot.state != nightglass::update::UpdateState::receiving ||
                snapshot.received_bytes != snapshot.expected_bytes) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update is not complete"};
            } else {
                // The companion request is already authenticated and the
                // manifest/signature/image have been checked. Apply the
                // validated inactive slot immediately, then give the status
                // notification a short window to leave over BLE before the
                // controlled reboot. Rollback still protects the next boot.
                result = service.finish();
                // Automatic installs have no confirmation surface. The
                // authenticated request plus the verified signed package is
                // the approval, so do not briefly render the legacy
                // INSTALL/RESTART overlay before the controlled reboot.
                publish_transport_snapshot(false, false);
                restart_after_status = result.is_ok();
            }
            break;
        case UpdateTransportCommandKind::abort:
            if (active_session != 0 &&
                (active_session != command.session || active_link != command.link)) {
                result = {nightglass::core::StatusCode::invalid_state,
                          "update abort session mismatch"};
            } else {
                result = service.abort();
                if (result.is_ok()) {
                    active_session = 0;
                    active_link = UpdateTransportLink::none;
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
    if (active_session == 0 ||
        (active_session == command.session && active_link == command.link)) {
        last_command_us = esp_timer_get_time();
    }
    const auto code = result_code(result);
    publish_status(command.link, command.session, code, command.kind);
    if (restart_after_status) {
        // Both USB and NimBLE queue the status from publish_status(); do not
        // reset the transport before that acknowledgement can be delivered.
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI("nightglass_update", "UPDATE_AUTO_RESTART session=%llu link=%u",
                 static_cast<unsigned long long>(command.session),
                 static_cast<unsigned>(command.link));
        esp_restart();
    }
}

void worker(void *) {
    UpdateTransportCommand command{};
    while (true) {
        if (xQueueReceive(command_queue, &command, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (!connection_epoch_current(command)) {
                wipe(&command, sizeof(command));
                continue;
            }
            handle(command);
            wipe(&command, sizeof(command));
        } else if (active_session != 0) {
            const auto now = esp_timer_get_time();
            portENTER_CRITICAL(&transport_mux);
            const bool active_connected = valid_link(active_link) &&
                                          link_connected[link_index(active_link)];
            const bool disconnected_too_long = !active_connected && link_lost_us > 0 &&
                                               now - link_lost_us > 30'000'000;
            portEXIT_CRITICAL(&transport_mux);
            if (disconnected_too_long ||
                (last_command_us > 0 && now - last_command_us > 120'000'000)) {
                const auto abort_result = nightglass::update::update_service().abort();
                if (abort_result.is_ok()) {
                    active_session = 0;
                    active_link = UpdateTransportLink::none;
                    active_manifest = {};
                    publish_transport_snapshot(false);
                    voice_service().update_finished();
                }
            }
        }
    }
}

}  // namespace

nightglass::core::Status UpdateTransport::start(UpdateTransportLink link,
                                                UpdateStatusSink sink) {
    if (!valid_link(link) || sink == nullptr) {
        return {nightglass::core::StatusCode::invalid_state,
                "update status sink unavailable"};
    }
    portENTER_CRITICAL(&transport_mux);
    status_sinks[link_index(link)] = sink;
    portEXIT_CRITICAL(&transport_mux);
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
    if (command_queue == nullptr || !valid_link(command.link)) return false;
    auto stamped = command;
    portENTER_CRITICAL(&transport_mux);
    const bool connected = link_connected[link_index(command.link)];
    stamped.connection_epoch = link_epochs[link_index(command.link)];
    portEXIT_CRITICAL(&transport_mux);
    return connected && stamped.connection_epoch != 0 &&
           xQueueSend(command_queue, &stamped, 0) == pdTRUE;
}

bool UpdateTransport::abort() noexcept {
    UpdateTransportCommand command{};
    command.kind = UpdateTransportCommandKind::abort;
    portENTER_CRITICAL(&transport_mux);
    command.session = transport_snapshot.session;
    command.link = transport_snapshot.link;
    if (valid_link(command.link)) {
        command.connection_epoch = link_epochs[link_index(command.link)];
    }
    portEXIT_CRITICAL(&transport_mux);
    return command.session != 0 && command.connection_epoch != 0 &&
           command_queue != nullptr && xQueueSend(command_queue, &command, 0) == pdTRUE;
}

void UpdateTransport::link_ready(UpdateTransportLink link) noexcept {
    if (!valid_link(link)) return;
    portENTER_CRITICAL(&transport_mux);
    if (++link_epochs[link_index(link)] == 0) ++link_epochs[link_index(link)];
    link_connected[link_index(link)] = true;
    if (transport_snapshot.session != 0 && transport_snapshot.link == link) {
        link_lost_us = 0;
    }
    portEXIT_CRITICAL(&transport_mux);
}

void UpdateTransport::link_lost(UpdateTransportLink link) noexcept {
    if (!valid_link(link)) return;
    portENTER_CRITICAL(&transport_mux);
    if (++link_epochs[link_index(link)] == 0) ++link_epochs[link_index(link)];
    link_connected[link_index(link)] = false;
    if (transport_snapshot.session != 0 && transport_snapshot.link == link) {
        link_lost_us = esp_timer_get_time();
    }
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
