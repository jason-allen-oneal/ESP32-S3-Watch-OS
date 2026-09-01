#include "nightglass/update/verifier.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kPublicPoint =
    "04f193c27046bd5086423c957dc4371fe454b0714ffdc3e1654d4cb6d22277cb30"
    "f6344f81746bbce9120dd498bff9c1adb6b9133fa23538aa4ebec773f755c406";
constexpr std::string_view kWrongPublicPoint =
    "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
constexpr std::string_view kOffCurvePoint =
    "040000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kPayload =
    "NIGHTGLASS-OTA-MANIFEST\n"
    "format=1\n"
    "board=nightglass-waveshare-esp32s3r8-2.06-v1.0\n"
    "partition=nightglass-32m-r1\n"
    "partition_revision=1\n"
    "version=v0.3.0\n"
    "secure_version=2\n"
    "size=1024\n"
    "sha256=abababababababababababababababababababababababababababababababab\n";

std::vector<std::uint8_t> decode_hex(std::string_view encoded) {
    auto value = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    assert((encoded.size() % 2) == 0);
    std::vector<std::uint8_t> output(encoded.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = value(encoded[index * 2]);
        const int low = value(encoded[index * 2 + 1]);
        assert(high >= 0 && low >= 0);
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return output;
}

std::span<const std::uint8_t> payload_bytes(std::string_view payload) {
    return {reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()};
}

std::vector<std::uint8_t> read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char **argv) {
    using namespace nightglass::update;

    if (argc == 5 && std::string_view(argv[1]) == "--verify-files") {
        P256SignatureVerifier file_verifier(argv[4]);
        assert(file_verifier.initialize().is_ok());
        const auto payload = read_file(argv[2]);
        const auto signature = read_file(argv[3]);
        assert(file_verifier.verify(payload, signature) == SignatureResult::verified);
        return 0;
    }

    P256SignatureVerifier absent("");
    assert(absent.initialize().code == nightglass::core::StatusCode::unavailable);
    assert(absent.verify(payload_bytes(kPayload), {}) == SignatureResult::unavailable);

    P256SignatureVerifier malformed("04ff");
    assert(malformed.initialize().code == nightglass::core::StatusCode::invalid_state);
    assert(malformed.verify(payload_bytes(kPayload), {}) == SignatureResult::rejected);

    P256SignatureVerifier off_curve(kOffCurvePoint);
    assert(off_curve.initialize().code == nightglass::core::StatusCode::invalid_state);

    P256SignatureVerifier verifier(kPublicPoint);
    assert(verifier.initialize().is_ok());
    assert(verifier.initialize().is_ok());

    const auto low_signature = decode_hex(
        "3045022100af011ff41637f55d03a3443d0e782de7df880234eee314fabc52b46f"
        "fe90110a0220154185da6a2bb9ca1eb07da716a2e17c7c84b666438da731adc33d"
        "4fd70e3617");
    const auto high_signature = decode_hex(
        "3046022100af011ff41637f55d03a3443d0e782de7df880234eee314fabc52b46f"
        "fe90110a022100eabe7a2495d44636e14f8258e95d1e83406244476389f75345f6"
        "8d732554ef3a");
    assert(verifier.verify(payload_bytes(kPayload), low_signature) ==
           SignatureResult::verified);
    assert(verifier.verify(payload_bytes(kPayload), high_signature) ==
           SignatureResult::verified);

    auto changed_payload = std::string(kPayload);
    changed_payload[changed_payload.size() - 2] ^= 1;
    assert(verifier.verify(payload_bytes(changed_payload), low_signature) ==
           SignatureResult::rejected);

    auto flipped_signature = low_signature;
    flipped_signature.back() ^= 1;
    assert(verifier.verify(payload_bytes(kPayload), flipped_signature) ==
           SignatureResult::rejected);

    P256SignatureVerifier wrong_key(kWrongPublicPoint);
    assert(wrong_key.initialize().is_ok());
    assert(wrong_key.verify(payload_bytes(kPayload), low_signature) ==
           SignatureResult::rejected);

    auto truncated = low_signature;
    truncated.pop_back();
    assert(verifier.verify(payload_bytes(kPayload), truncated) ==
           SignatureResult::rejected);
    auto trailing = low_signature;
    trailing.push_back(0);
    assert(verifier.verify(payload_bytes(kPayload), trailing) ==
           SignatureResult::rejected);
    std::array<std::uint8_t, 81> overlong{};
    assert(verifier.verify(payload_bytes(kPayload), overlong) ==
           SignatureResult::rejected);
    for (const auto malformed_der : {
             "3006020100020101",      // zero r
             "3006020180020101",      // negative r
             "300702020001020101",    // non-minimal INTEGER
             "308145020101020101",    // malformed long-form length
             "3049020101020101",      // declared length exceeds input
         }) {
        const auto malformed_signature = decode_hex(malformed_der);
        assert(verifier.verify(payload_bytes(kPayload), malformed_signature) ==
               SignatureResult::rejected);
    }
}
