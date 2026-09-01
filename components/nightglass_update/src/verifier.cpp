#include "nightglass/update/verifier.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

namespace nightglass::update {
namespace {

constexpr std::size_t kPublicPointBytes = 65;
constexpr std::size_t kPublicPointHexCharacters = kPublicPointBytes * 2;
constexpr std::size_t kSha256Bytes = 32;

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decode_public_point(std::string_view encoded,
                         std::array<std::uint8_t, kPublicPointBytes> &point) noexcept {
    if (encoded.size() != kPublicPointHexCharacters) return false;
    for (std::size_t index = 0; index < point.size(); ++index) {
        const int high = hex_value(encoded[index * 2]);
        const int low = hex_value(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        point[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return point[0] == 0x04;
}

P256SignatureVerifier verifier{CONFIG_NIGHTGLASS_OTA_P256_PUBLIC_KEY_HEX};

}  // namespace

P256SignatureVerifier::P256SignatureVerifier(std::string_view encoded_public_point) noexcept
    : encoded_public_point_(encoded_public_point) {}

nightglass::core::Status P256SignatureVerifier::initialize() noexcept {
    if (initialized_) {
        return available_
                   ? nightglass::core::Status::Ok()
                   : nightglass::core::Status{
                         configured() ? nightglass::core::StatusCode::invalid_state
                                      : nightglass::core::StatusCode::unavailable,
                         configured() ? "OTA public key is invalid"
                                      : "OTA public key is not provisioned"};
    }
    initialized_ = true;
    if (!configured()) {
        return {nightglass::core::StatusCode::unavailable,
                "OTA public key is not provisioned"};
    }
    if (!decode_public_point(encoded_public_point_, public_point_)) {
        return {nightglass::core::StatusCode::invalid_state,
                "OTA public key encoding is invalid"};
    }

    mbedtls_ecdsa_context context;
    mbedtls_ecdsa_init(&context);
    int result = mbedtls_ecp_group_load(&context.MBEDTLS_PRIVATE(grp),
                                        MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_ecp_point_read_binary(
            &context.MBEDTLS_PRIVATE(grp), &context.MBEDTLS_PRIVATE(Q),
            public_point_.data(), public_point_.size());
    }
    if (result == 0) {
        result = mbedtls_ecp_check_pubkey(&context.MBEDTLS_PRIVATE(grp),
                                          &context.MBEDTLS_PRIVATE(Q));
    }
    mbedtls_ecdsa_free(&context);
    if (result != 0) {
        public_point_.fill(0);
        return {nightglass::core::StatusCode::invalid_state,
                "OTA public key is not a valid P-256 point"};
    }
    available_ = true;
    return nightglass::core::Status::Ok();
}

bool P256SignatureVerifier::configured() const noexcept {
    return !encoded_public_point_.empty();
}

bool P256SignatureVerifier::available() const noexcept {
    return initialized_ && available_;
}

SignatureResult P256SignatureVerifier::verify(
    std::span<const std::uint8_t> canonical_manifest,
    std::span<const std::uint8_t> signature) const noexcept {
    if (!configured()) return SignatureResult::unavailable;
    if (!available() || canonical_manifest.empty() || signature.empty() ||
        signature.size() > 80) {
        return SignatureResult::rejected;
    }

    std::array<std::uint8_t, kSha256Bytes> digest{};
    if (mbedtls_sha256(canonical_manifest.data(), canonical_manifest.size(),
                       digest.data(), 0) != 0) {
        return SignatureResult::rejected;
    }

    mbedtls_ecdsa_context context;
    mbedtls_ecdsa_init(&context);
    int result = mbedtls_ecp_group_load(&context.MBEDTLS_PRIVATE(grp),
                                        MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_ecp_point_read_binary(
            &context.MBEDTLS_PRIVATE(grp), &context.MBEDTLS_PRIVATE(Q),
            public_point_.data(), public_point_.size());
    }
    if (result == 0) {
        result = mbedtls_ecp_check_pubkey(&context.MBEDTLS_PRIVATE(grp),
                                          &context.MBEDTLS_PRIVATE(Q));
    }
    if (result == 0) {
        result = mbedtls_ecdsa_read_signature(&context, digest.data(), digest.size(),
                                              signature.data(), signature.size());
    }
    mbedtls_ecdsa_free(&context);
    return result == 0 ? SignatureResult::verified : SignatureResult::rejected;
}

nightglass::core::Status initialize_provisioned_signature_verifier() noexcept {
    return verifier.initialize();
}

const SignatureVerifier *provisioned_signature_verifier() noexcept {
    return verifier.configured() ? &verifier : nullptr;
}

}  // namespace nightglass::update
