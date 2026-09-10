#include "nightglass/update/policy.hpp"

namespace nightglass::update {

BootDecision evaluate_boot(std::uint32_t unhealthy_boots, bool recovery_button_held,
                           bool pending_verification) noexcept {
    // PENDING_VERIFY is already a bootloader-enforced one-shot attempt. Let a
    // repaired image prove itself even when the previous image exhausted the
    // software boot counter; the physical recovery request still wins.
    return {
        .safe_mode = recovery_button_held ||
                     (unhealthy_boots >= kUnhealthyBootLimit && !pending_verification),
        .pending_verification = pending_verification,
        .unhealthy_boots = unhealthy_boots,
    };
}

HealthGateAction evaluate_health_gate(const BootDecision &boot,
                                      bool essential_healthy) noexcept {
    if (boot.pending_verification) {
        return essential_healthy && !boot.safe_mode
                   ? HealthGateAction::accept_pending_image
                   : HealthGateAction::rollback_pending_image;
    }
    return essential_healthy ? HealthGateAction::record_healthy
                             : HealthGateAction::retain_unhealthy;
}

}  // namespace nightglass::update
