#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"

namespace nightglass::core {

enum class HealthState : std::uint8_t { unknown, ok, degraded, absent, failed };

struct HealthRecord {
    bool occupied{false};
    std::array<char, 24> component{};
    HealthState state{HealthState::unknown};
    std::array<char, 96> detail{};
    std::uint32_t failure_count{0};
    std::int64_t last_success_us{0};
};

class HealthRegistry {
public:
    static constexpr std::size_t capacity = 24;

    bool set(const char *component, HealthState state, const char *detail);
    [[nodiscard]] bool copy(const char *component, HealthRecord &destination) const;
    [[nodiscard]] std::array<HealthRecord, capacity> snapshot() const;

private:
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    std::array<HealthRecord, capacity> records_{};
};

HealthRegistry &health_registry();

}  // namespace nightglass::core
