#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace morrow::core {

enum class HealthState : std::uint8_t { unknown, ok, degraded, absent, failed };

struct HealthRecord {
    const char *component{nullptr};
    HealthState state{HealthState::unknown};
    const char *detail{"not probed"};
    std::uint32_t failure_count{0};
    std::int64_t last_success_us{0};
};

class HealthRegistry {
public:
    static constexpr std::size_t capacity = 24;

    bool set(const char *component, HealthState state, const char *detail);
    [[nodiscard]] const HealthRecord *find(const char *component) const;
    [[nodiscard]] const std::array<HealthRecord, capacity> &records() const { return records_; }

private:
    std::array<HealthRecord, capacity> records_{};
};

HealthRegistry &health_registry();

}  // namespace morrow::core
