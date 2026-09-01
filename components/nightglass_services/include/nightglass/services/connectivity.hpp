#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

#include "nightglass/core/status.hpp"
#include "nightglass/services/connectivity_protocol.hpp"

namespace nightglass::services {

enum class CompanionLinkState : std::uint8_t {
    disabled,
    advertising,
    connected_unsecured,
    connected_encrypted,
    failed,
};

struct ConnectivitySettings {
    bool enabled{false};
    std::array<char, 25> device_name{"Nightglass"};
};

enum class NotificationPrivacyPolicy : std::uint8_t {
    show_details,
    redact_when_locked,
    always_redact,
};

struct ConnectivitySnapshot {
    std::uint32_t sequence{0};
    std::uint32_t notification_sequence{0};
    ConnectivitySettings settings{};
    CompanionLinkState state{CompanionLinkState::disabled};
    bool bonded{false};
    bool encrypted{false};
    std::uint8_t notification_count{0};
    std::array<CompanionNotification, kNotificationCapacity> notifications{};
    CompanionMediaState media{};
    CompanionAgenda agenda{};
    CompanionPhoneBattery phone_battery{};
    CompanionCallState call{};
    std::uint32_t reply_notification_id{0};
    std::uint32_t reply_nonce{0};
    std::uint8_t reply_status{0};
    bool reply_pending{false};
    std::uint32_t pairing_passkey{0};
    bool pairing_passkey_active{false};
    bool peer_identity_pinned{false};
    NotificationPrivacyPolicy notification_privacy{NotificationPrivacyPolicy::show_details};
    bool notification_details_unlocked{true};
    std::array<char, 64> detail{"Not started"};
};

static_assert(std::is_trivially_copyable_v<ConnectivitySnapshot>);

class ConnectivityService {
public:
    nightglass::core::Status start();
    [[nodiscard]] ConnectivitySnapshot snapshot() const;
    nightglass::core::Status update_settings(const ConnectivitySettings &settings);
    bool send_media(MediaCommand command);
    bool send_call(CallCommand command);
    bool send_phone(PhoneCommand command);
    bool mark_notification(std::uint32_t id, bool dismiss);
    bool reply_notification(std::uint32_t id, const char *reply);
    bool send_voice_frame(std::span<const std::uint8_t> frame);
    [[nodiscard]] std::size_t maximum_outbound_frame() const;
    // Runtime-only hook for a future PIN/privacy UI. Tightening the policy
    // immediately scrubs cached notification text; relaxing it does not
    // resurrect prior content and requires a fresh companion sync.
    void set_notification_privacy(NotificationPrivacyPolicy policy, bool unlocked);
};

ConnectivityService &connectivity_service();

}  // namespace nightglass::services
