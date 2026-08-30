#pragma once

#include <array>
#include <cstdint>
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
    bool enabled{true};
    std::array<char, 25> device_name{"Nightglass"};
};

struct ConnectivitySnapshot {
    std::uint32_t sequence{0};
    ConnectivitySettings settings{};
    CompanionLinkState state{CompanionLinkState::disabled};
    bool bonded{false};
    bool encrypted{false};
    std::uint8_t notification_count{0};
    std::array<CompanionNotification, kNotificationCapacity> notifications{};
    std::array<char, 64> detail{"Not started"};
};

static_assert(std::is_trivially_copyable_v<ConnectivitySnapshot>);

class ConnectivityService {
public:
    nightglass::core::Status start();
    [[nodiscard]] ConnectivitySnapshot snapshot() const;
    nightglass::core::Status update_settings(const ConnectivitySettings &settings);
    bool send_media(MediaCommand command);
    bool mark_notification(std::uint32_t id, bool dismiss);
};

ConnectivityService &connectivity_service();

}  // namespace nightglass::services
