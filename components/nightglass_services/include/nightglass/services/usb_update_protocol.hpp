#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nightglass/services/update_transport_protocol.hpp"

namespace nightglass::services {

inline constexpr std::array<std::uint8_t, 4> kUsbUpdateEnvelopeMagic{
    'N', 'G', 'U', '1'};
inline constexpr std::size_t kUsbUpdateEnvelopeHeaderSize = 6;
inline constexpr std::size_t kUsbUpdateEnvelopeChecksumSize = 4;
inline constexpr std::size_t kUsbUpdateEnvelopeMaximum =
    kUsbUpdateEnvelopeHeaderSize + kUsbUpdateTransportFrameMaximum +
    kUsbUpdateEnvelopeChecksumSize;

struct UsbUpdateEnvelope {
    std::array<std::uint8_t, kUsbUpdateEnvelopeMaximum> bytes{};
    std::size_t size{0};
};

[[nodiscard]] std::uint32_t usb_update_crc32(
    std::span<const std::uint8_t> payload) noexcept;
[[nodiscard]] UsbUpdateEnvelope encode_usb_update_envelope(
    std::span<const std::uint8_t> payload) noexcept;

class UsbUpdateEnvelopeDecoder {
public:
    // Returns true exactly once for each complete, checksum-valid envelope.
    [[nodiscard]] bool feed(std::uint8_t byte) noexcept;
    [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept;
    [[nodiscard]] std::uint32_t rejected_frames() const noexcept;
    [[nodiscard]] bool in_progress() const noexcept;
    // Abandon an incomplete envelope after a disconnect or assembly timeout.
    // The cumulative rejection counter is intentionally retained.
    void reset() noexcept;

private:
    enum class Stage : std::uint8_t { magic, length, payload, checksum };

    void reset_parser() noexcept;

    Stage stage_{Stage::magic};
    std::size_t magic_offset_{0};
    std::size_t length_offset_{0};
    std::size_t payload_offset_{0};
    std::size_t checksum_offset_{0};
    std::uint16_t expected_length_{0};
    std::uint32_t supplied_checksum_{0};
    std::size_t ready_length_{0};
    std::uint32_t rejected_frames_{0};
    std::array<std::uint8_t, kUsbUpdateTransportFrameMaximum> payload_{};
};

}  // namespace nightglass::services
