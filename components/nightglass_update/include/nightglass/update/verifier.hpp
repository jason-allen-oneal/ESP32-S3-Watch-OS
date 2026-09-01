#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "nightglass/core/status.hpp"
#include "nightglass/update/service.hpp"

namespace nightglass::update {

class P256SignatureVerifier final : public SignatureVerifier {
public:
    explicit P256SignatureVerifier(std::string_view encoded_public_point) noexcept;

    // Performs curve loading and public-point validation explicitly during
    // boot. The constructor deliberately performs no mbedTLS work.
    [[nodiscard]] nightglass::core::Status initialize() noexcept;
    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] SignatureResult verify(
        std::span<const std::uint8_t> canonical_manifest,
        std::span<const std::uint8_t> signature) const noexcept override;

private:
    static constexpr std::size_t kPublicPointBytes = 65;

    std::string_view encoded_public_point_{};
    std::array<std::uint8_t, kPublicPointBytes> public_point_{};
    bool initialized_{false};
    bool available_{false};
};

// Validates the configured production key during boot. An empty key returns
// unavailable; a malformed or off-curve key returns invalid_state and leaves
// a rejecting sentinel installed.
[[nodiscard]] nightglass::core::Status initialize_provisioned_signature_verifier() noexcept;

// Returns nullptr only when no key text was configured. A configured but
// invalid key returns a rejecting verifier so development unsigned mode cannot
// mistake provisioning failure for an absent verifier.
[[nodiscard]] const SignatureVerifier *provisioned_signature_verifier() noexcept;

}  // namespace nightglass::update
