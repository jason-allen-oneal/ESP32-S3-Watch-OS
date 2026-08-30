#pragma once

#include <array>
#include <cstdint>

namespace morrow::core {

enum class EventTopic : std::uint16_t {
    health_changed,
    power_changed,
    time_changed,
    motion_changed,
    connectivity_changed,
    notification_received,
    alarm_fired,
};

struct Event {
    EventTopic topic{};
    std::uint16_t source{};
    std::uint32_t sequence{};
    std::int64_t timestamp_us{};
    std::array<std::uint8_t, 16> payload{};
};

static_assert(sizeof(Event) <= 32, "Events must stay fixed and queue-friendly");

}  // namespace morrow::core
