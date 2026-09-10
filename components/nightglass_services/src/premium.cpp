#include "nightglass/services/premium.hpp"

#include <algorithm>
#include <new>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nightglass/services/connectivity.hpp"
#include "nightglass/services/power.hpp"

namespace nightglass::services {
namespace {
PremiumService instance;
PremiumProfile current;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
StaticSemaphore_t save_mutex_storage;
SemaphoreHandle_t save_mutex{};
PremiumTransfer *transfer{}; // Explicit PSRAM; never grow BLE or power stacks.
std::int64_t requested_us{};
std::uint8_t last_action_status{0xff};
std::uint8_t action_sequence{};
void put32(std::uint8_t *p, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8 * i);
}
}
void PremiumService::start() {
    if (save_mutex) return;
    save_mutex = xSemaphoreCreateMutexStatic(&save_mutex_storage);
    auto *storage = heap_caps_calloc(1, sizeof(PremiumTransfer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage) transfer = new (storage) PremiumTransfer{};
    nvs_handle_t handle{};
    // Preserve pre-premium face selection on migration; never rewrite it at boot.
    if (nvs_open("ng_face", NVS_READONLY, &handle) == ESP_OK) {
        std::uint8_t old_face{};
        if (nvs_get_u8(handle, "selected", &old_face) == ESP_OK && old_face <= 1)
            current.face = old_face;
        nvs_close(handle);
    }
    if (nvs_open("ng_premium", NVS_READONLY, &handle) != ESP_OK) return;
    std::array<std::uint8_t, kPremiumProfileBytes> data{};
    std::size_t size = data.size();
    PremiumProfile p{}; std::uint32_t nonce{};
    if (nvs_get_blob(handle, "profile", data.data(), &size) == ESP_OK &&
        decode_premium_profile(std::span(data.data(), size), p, nonce)) current = p;
    nvs_close(handle);
}
PremiumProfile PremiumService::profile() const {
    portENTER_CRITICAL(&lock); const auto p = current; portEXIT_CRITICAL(&lock);
    return p;
}
bool PremiumService::save(const PremiumProfile &p) {
    if (!valid_premium_profile(p) || !save_mutex ||
        xSemaphoreTake(save_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool ok = p == profile();
    if (!ok) {
        const auto frame = encode_premium_profile(p, 0);
        nvs_handle_t handle{};
        if (nvs_open("ng_premium", NVS_READWRITE, &handle) == ESP_OK) {
            ok = nvs_set_blob(handle, "profile", frame.data(), frame.size()) == ESP_OK &&
                 nvs_commit(handle) == ESP_OK;
            nvs_close(handle);
        }
        if (ok) {
            portENTER_CRITICAL(&lock); current = p; portEXIT_CRITICAL(&lock);
        }
    }
    xSemaphoreGive(save_mutex);
    return ok;
}
bool PremiumService::accept(std::span<const std::uint8_t> f) {
    if (f.size() < 2 || f[0] != 1) return false;
    if (f[1] == 0x71 && f.size() == 2) {
        const auto reply = encode_premium_profile(profile(), 0, true);
        return connectivity_service().send_premium_frame(reply);
    }
    if (f[1] == 0x70) {
        PremiumProfile p{}; std::uint32_t nonce{};
        if (!decode_premium_profile(f, p, nonce) || !nonce || !save(p)) return false;
        const auto reply = encode_premium_profile(p, nonce, true);
        (void)connectivity_service().send_premium_frame(reply);
        // Reuse the existing power supervisor; no extra worker/task is needed.
        power_service().note_activity(nightglass::core::WakeReason::notification);
        return true;
    }
    portENTER_CRITICAL(&lock);
    bool ok = false;
    if (transfer && esp_timer_get_time() - requested_us < 60'000'000) {
        if (f[1] == 0x77 && f.size() == 8) {
            const auto token = transfer->token();
            ok = f[2] == (token & 255) && f[3] == ((token >> 8) & 255) &&
                 f[4] == ((token >> 16) & 255) && f[5] == (token >> 24) &&
                 f[6] == action_sequence && f[7] <= 5;
            if (ok) last_action_status = f[7];
        } else ok = transfer->accept(f);
    }
    portEXIT_CRITICAL(&lock);
    return ok;
}
bool PremiumService::request_content(std::uint8_t kind, std::uint32_t target) {
    if (!transfer || kind < 1 || kind > 2 || (kind == 2 && !target)) return false;
    if (kind == 2) {
        const auto connection = connectivity_service().snapshot();
        if (connection.notification_privacy != NotificationPrivacyPolicy::show_details ||
            !connection.notification_details_unlocked ||
            std::none_of(connection.notifications.begin(), connection.notifications.end(),
                [target](const auto &n) { return n.valid && n.id == target; })) return false;
    }
    auto token = esp_random(); if (!token) token = 1;
    std::array<std::uint8_t, 11> f{1, 0x72};
    put32(f.data() + 2, token); f[6] = kind; put32(f.data() + 7, target);
    portENTER_CRITICAL(&lock);
    transfer->request(token, kind, target); requested_us = esp_timer_get_time();
    last_action_status = 0xff;
    portEXIT_CRITICAL(&lock);
    if (connectivity_service().send_premium_frame(f)) return true;
    clear_content(); return false;
}
std::size_t PremiumService::copy_content(std::span<std::uint8_t> dst,
    std::uint32_t &token, std::uint8_t &kind, std::uint32_t &target) const {
    portENTER_CRITICAL(&lock);
    std::size_t size = 0;
    if (transfer && esp_timer_get_time() - requested_us < 60'000'000) {
        const auto data = transfer->content();
        if (!data.empty() && data.size() <= dst.size()) {
            std::copy(data.begin(), data.end(), dst.begin()); size = data.size();
            token = transfer->token(); kind = transfer->kind(); target = transfer->target();
        }
    }
    portEXIT_CRITICAL(&lock);
    return size;
}
bool PremiumService::action(std::uint8_t action, std::uint8_t index) {
    std::array<std::uint8_t, 13> f{1, 0x76};
    portENTER_CRITICAL(&lock);
    const bool ready = transfer && !transfer->content().empty() &&
                       esp_timer_get_time() - requested_us < 60'000'000;
    if (ready) {
        put32(f.data() + 2, transfer->token()); f[6] = action; f[7] = index;
        put32(f.data() + 8, transfer->target());
        if (++action_sequence == 0) ++action_sequence;
        f[12] = action_sequence; last_action_status = 0xfe;
    }
    portEXIT_CRITICAL(&lock);
    const bool sent = ready && connectivity_service().send_premium_frame(f);
    if (!sent) { portENTER_CRITICAL(&lock); last_action_status = 5; portEXIT_CRITICAL(&lock); }
    return sent;
}
void PremiumService::clear_content() {
    portENTER_CRITICAL(&lock);
    if (transfer) transfer->clear();
    requested_us = 0; last_action_status = 0xff;
    portEXIT_CRITICAL(&lock);
}
std::uint8_t PremiumService::action_status() const {
    portENTER_CRITICAL(&lock); const auto s = last_action_status; portEXIT_CRITICAL(&lock);
    return s;
}
PremiumService &premium_service() { return instance; }
}  // namespace nightglass::services
