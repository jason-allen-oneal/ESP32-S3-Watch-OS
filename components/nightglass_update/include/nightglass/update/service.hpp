#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nightglass/core/status.hpp"
#include "nightglass/update/manifest.hpp"

namespace nightglass::update {

enum class SignatureResult : std::uint8_t { verified, rejected, unavailable };

class SignatureVerifier {
public:
    virtual ~SignatureVerifier() = default;
    [[nodiscard]] virtual SignatureResult verify(
        std::span<const std::uint8_t> canonical_manifest,
        std::span<const std::uint8_t> signature) const noexcept = 0;
};

enum class UpdateState : std::uint8_t {
    disabled,
    idle,
    receiving,
    ready_to_reboot,
    failed,
};

enum class SignatureState : std::uint8_t {
    not_checked,
    verified,
    unsigned_development,
    rejected,
    verifier_unavailable,
};

struct UpdateSnapshot {
    UpdateState state{UpdateState::disabled};
    SignatureState signature_state{SignatureState::not_checked};
    bool safe_mode{false};
    bool pending_verification{false};
    bool health_gate_armed{false};
    std::uint32_t unhealthy_boots{0};
    std::uint32_t expected_bytes{0};
    std::uint32_t received_bytes{0};
    std::array<char, 32> target_version{};
    std::array<char, 16> target_partition{};
};

class UpdateService {
public:
    // Call after NVS initialization. A held side key is sampled separately so
    // normal GPIO ownership remains with PowerService.
    nightglass::core::Status begin_boot(bool nvs_available, bool recovery_button_held);
    nightglass::core::Status arm_health_gate();

    // The verifier object must have static lifetime. With no verifier, the
    // production-default policy refuses all images before erasing a slot.
    nightglass::core::Status set_signature_verifier(const SignatureVerifier *verifier);

    nightglass::core::Status begin_update(
        const UpdateManifest &manifest,
        std::span<const std::uint8_t> signature);
    nightglass::core::Status write(std::span<const std::uint8_t> chunk);
    nightglass::core::Status finish();
    nightglass::core::Status abort();

    [[nodiscard]] UpdateSnapshot snapshot() const;
};

// Samples GPIO10 only when initially asserted. It returns true only if the
// side key stays held for the complete recovery window.
[[nodiscard]] bool recovery_button_held_at_boot();
UpdateService &update_service();

}  // namespace nightglass::update
