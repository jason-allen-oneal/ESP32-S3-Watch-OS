#pragma once

#include <cstdint>

namespace nightglass::update {

inline constexpr std::uint32_t kUnhealthyBootLimit = 3;
inline constexpr std::uint32_t kHealthGateSeconds = 60;

struct BootDecision {
    bool safe_mode{false};
    bool pending_verification{false};
    std::uint32_t unhealthy_boots{0};
};

enum class HealthGateAction : std::uint8_t {
    retain_unhealthy,
    record_healthy,
    accept_pending_image,
    rollback_pending_image,
};

[[nodiscard]] BootDecision evaluate_boot(std::uint32_t unhealthy_boots,
                                         bool recovery_button_held,
                                         bool pending_verification) noexcept;
[[nodiscard]] HealthGateAction evaluate_health_gate(const BootDecision &boot,
                                                      bool essential_healthy) noexcept;

}  // namespace nightglass::update
