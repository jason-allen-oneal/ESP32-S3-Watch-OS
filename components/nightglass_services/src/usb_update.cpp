#include "nightglass/services/usb_update.hpp"

#include <array>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nightglass/core/health.hpp"
#include "nightglass/services/update_transport.hpp"
#include "nightglass/services/usb_update_protocol.hpp"
#include "nightglass/update/service.hpp"

namespace nightglass::services {
namespace {

constexpr char kTag[] = "nightglass_usb_update";
constexpr std::size_t kRxBufferSize = 1024;
constexpr std::size_t kTxBufferSize = 2048;
// The host pipelines at most three frames. Keep ISR buffers internal, but
// reserve scarce internal RAM for the radio and the flash-writing worker.
static_assert(kRxBufferSize >= 3 * kUsbUpdateEnvelopeMaximum);
constexpr TickType_t kReadWait = pdMS_TO_TICKS(100);
constexpr TickType_t kDisconnectedReadWait = pdMS_TO_TICKS(1000);
constexpr TickType_t kWriteWait = pdMS_TO_TICKS(250);
constexpr std::int64_t kEnvelopeAssemblyTimeoutUs = 1'000'000;

UsbUpdateService instance;
TaskHandle_t receiver_task{};
portMUX_TYPE snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
UsbUpdateSnapshot current{};
UsbUpdateEnvelopeDecoder decoder{};

bool set_connected(bool connected) {
    bool changed = false;
    portENTER_CRITICAL(&snapshot_mux);
    if (current.connected != connected) {
        current.connected = connected;
        changed = true;
    }
    portEXIT_CRITICAL(&snapshot_mux);
    if (!changed) return false;
    decoder.reset();
    if (connected) {
        update_transport().link_ready(UpdateTransportLink::usb);
        ESP_LOGI(kTag, "USB_UPDATE_CONNECTED");
    } else {
        update_transport().link_lost(UpdateTransportLink::usb);
        ESP_LOGI(kTag, "USB_UPDATE_DISCONNECTED");
    }
    return true;
}

bool send_status(const std::uint8_t *data, std::size_t size) {
    if (data == nullptr || size == 0 ||
        !usb_serial_jtag_is_driver_installed() ||
        !usb_serial_jtag_is_connected()) {
        return false;
    }
    const auto update = nightglass::update::update_service().snapshot();
    if (update.pending_verification) {
        const auto *application = esp_app_get_description();
        ESP_LOGI(kTag,
                 "USB_UPDATE_BOOT version=%s secure=%lu pending=1 state=PENDING_VERIFY",
                 application->version,
                 static_cast<unsigned long>(application->secure_version));
    } else {
        static std::int64_t last_identity_us = 0;
        const auto now_us = esp_timer_get_time();
        if (last_identity_us == 0 || now_us - last_identity_us >= 10'000'000) {
            last_identity_us = now_us;
            const auto *application = esp_app_get_description();
            const auto *running = esp_ota_get_running_partition();
            esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
            const auto state_result = running
                ? esp_ota_get_state_partition(running, &state) : ESP_ERR_INVALID_STATE;
            ESP_LOGI(kTag, "USB_UPDATE_IDENTITY version=%s secure=%lu pending=0 partition=%s state_io=%s ota_state=%d",
                     application->version, static_cast<unsigned long>(application->secure_version),
                     running ? running->label : "unknown", esp_err_to_name(state_result),
                     state_result == ESP_OK ? static_cast<int>(state) : -1);
        }
    }
    const auto envelope = encode_usb_update_envelope(std::span(data, size));
    if (envelope.size == 0) return false;
    return usb_serial_jtag_write_bytes(envelope.bytes.data(), envelope.size,
                                       kWriteWait) ==
           static_cast<int>(envelope.size);
}

void receiver(void *) {
    std::array<std::uint8_t, 256> input{};
    std::uint32_t observed_rejections = 0;
    std::int64_t last_envelope_byte_us = 0;
    while (true) {
        const bool connected = usb_serial_jtag_is_connected();
        if (set_connected(connected)) last_envelope_byte_us = 0;
        const int count = usb_serial_jtag_read_bytes(input.data(), input.size(),
                                                     connected ? kReadWait
                                                               : kDisconnectedReadWait);
        const auto now_us = esp_timer_get_time();
        if (count <= 0) {
            if (decoder.in_progress() && last_envelope_byte_us > 0 &&
                now_us - last_envelope_byte_us > kEnvelopeAssemblyTimeoutUs) {
                decoder.reset();
                last_envelope_byte_us = 0;
                portENTER_CRITICAL(&snapshot_mux);
                ++current.rejected_frames;
                portEXIT_CRITICAL(&snapshot_mux);
            }
            continue;
        }
        for (int index = 0; index < count; ++index) {
            const bool ready = decoder.feed(input[static_cast<std::size_t>(index)]);
            last_envelope_byte_us = decoder.in_progress() ? now_us : 0;
            if (!ready) continue;
            UpdateTransportCommand command{};
            const bool parsed = parse_usb_update_transport_frame(decoder.payload(), command);
            command.link = UpdateTransportLink::usb;
            const bool queued = parsed && update_transport().enqueue(command);
            portENTER_CRITICAL(&snapshot_mux);
            if (queued) {
                ++current.received_frames;
            } else {
                ++current.rejected_frames;
            }
            portEXIT_CRITICAL(&snapshot_mux);
        }
        const auto rejected = decoder.rejected_frames();
        if (rejected != observed_rejections) {
            portENTER_CRITICAL(&snapshot_mux);
            current.rejected_frames += rejected - observed_rejections;
            portEXIT_CRITICAL(&snapshot_mux);
            observed_rejections = rejected;
        }
    }
}

}  // namespace

nightglass::core::Status UsbUpdateService::start() {
    if (receiver_task != nullptr) return nightglass::core::Status::Ok();

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config{
            .tx_buffer_size = kTxBufferSize,
            .rx_buffer_size = kRxBufferSize,
        };
        const auto install = usb_serial_jtag_driver_install(&config);
        if (install != ESP_OK) {
            nightglass::core::health_registry().set(
                "usb_update", nightglass::core::HealthState::failed,
                "USB Serial/JTAG driver unavailable");
            return {nightglass::core::StatusCode::unavailable,
                    "USB Serial/JTAG driver unavailable"};
        }
    }
    usb_serial_jtag_vfs_use_driver();

    const auto transport_status = update_transport().start(
        UpdateTransportLink::usb, send_status);
    if (!transport_status.is_ok()) {
        nightglass::core::health_registry().set(
            "usb_update", nightglass::core::HealthState::failed,
            transport_status.detail);
        return transport_status;
    }
    const bool connected = usb_serial_jtag_is_connected();
    portENTER_CRITICAL(&snapshot_mux);
    current.connected = connected;
    portEXIT_CRITICAL(&snapshot_mux);
    if (connected) update_transport().link_ready(UpdateTransportLink::usb);

    // This task only decodes frames and queues commands. All flash operations
    // remain on UpdateTransport's internal-RAM stack.
    if (xTaskCreatePinnedToCoreWithCaps(
            receiver, "nightglass_usb_update", 5120, nullptr, 5,
            &receiver_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        receiver_task = nullptr;
        if (connected) update_transport().link_lost(UpdateTransportLink::usb);
        portENTER_CRITICAL(&snapshot_mux);
        current.connected = false;
        portEXIT_CRITICAL(&snapshot_mux);
        nightglass::core::health_registry().set(
            "usb_update", nightglass::core::HealthState::failed,
            "USB update receiver task unavailable");
        return {nightglass::core::StatusCode::no_memory,
                "USB update receiver task unavailable"};
    }
    portENTER_CRITICAL(&snapshot_mux);
    current.started = true;
    portEXIT_CRITICAL(&snapshot_mux);
    nightglass::core::health_registry().set(
        "usb_update", nightglass::core::HealthState::ok,
        "Signed direct USB update receiver started");
    nightglass::core::health_registry().set(
        "update_transport", nightglass::core::HealthState::ok,
        "Signed update core supports direct USB and companion OTA");
    ESP_LOGI(kTag,
             "USB_UPDATE_READY protocol=1 signed_only=1 max_chunk=%u raw_flash=0",
             static_cast<unsigned>(kUsbUpdateDataMaximum));
    ESP_LOGI(kTag, "USB_UPDATE_MEMORY internal_free=%lu largest=%lu rx=%u tx=%u",
             static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(kRxBufferSize), static_cast<unsigned>(kTxBufferSize));
    return nightglass::core::Status::Ok();
}

UsbUpdateSnapshot UsbUpdateService::snapshot() const noexcept {
    portENTER_CRITICAL(&snapshot_mux);
    const auto copy = current;
    portEXIT_CRITICAL(&snapshot_mux);
    return copy;
}

UsbUpdateService &usb_update_service() { return instance; }

}  // namespace nightglass::services
