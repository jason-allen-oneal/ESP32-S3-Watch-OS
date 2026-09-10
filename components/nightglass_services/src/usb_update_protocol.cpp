#include "nightglass/services/usb_update_protocol.hpp"

#include <algorithm>

namespace nightglass::services {

std::uint32_t usb_update_crc32(std::span<const std::uint8_t> payload) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : payload) {
        crc ^= byte;
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

UsbUpdateEnvelope encode_usb_update_envelope(
    std::span<const std::uint8_t> payload) noexcept {
    UsbUpdateEnvelope envelope{};
    if (payload.empty() || payload.size() > kUsbUpdateTransportFrameMaximum) {
        return envelope;
    }
    std::copy(kUsbUpdateEnvelopeMagic.begin(), kUsbUpdateEnvelopeMagic.end(),
              envelope.bytes.begin());
    const auto length = static_cast<std::uint16_t>(payload.size());
    envelope.bytes[4] = static_cast<std::uint8_t>(length);
    envelope.bytes[5] = static_cast<std::uint8_t>(length >> 8U);
    std::copy(payload.begin(), payload.end(),
              envelope.bytes.begin() + kUsbUpdateEnvelopeHeaderSize);
    const auto checksum = usb_update_crc32(payload);
    const auto checksum_offset = kUsbUpdateEnvelopeHeaderSize + payload.size();
    for (std::size_t index = 0; index < kUsbUpdateEnvelopeChecksumSize; ++index) {
        envelope.bytes[checksum_offset + index] =
            static_cast<std::uint8_t>(checksum >> (index * 8U));
    }
    envelope.size = checksum_offset + kUsbUpdateEnvelopeChecksumSize;
    return envelope;
}

void UsbUpdateEnvelopeDecoder::reset_parser() noexcept {
    stage_ = Stage::magic;
    magic_offset_ = 0;
    length_offset_ = 0;
    payload_offset_ = 0;
    checksum_offset_ = 0;
    expected_length_ = 0;
    supplied_checksum_ = 0;
}

bool UsbUpdateEnvelopeDecoder::feed(std::uint8_t byte) noexcept {
    ready_length_ = 0;
    switch (stage_) {
        case Stage::magic:
            if (byte == kUsbUpdateEnvelopeMagic[magic_offset_]) {
                ++magic_offset_;
                if (magic_offset_ == kUsbUpdateEnvelopeMagic.size()) {
                    stage_ = Stage::length;
                    length_offset_ = 0;
                    expected_length_ = 0;
                }
            } else {
                magic_offset_ = byte == kUsbUpdateEnvelopeMagic[0] ? 1U : 0U;
            }
            return false;
        case Stage::length:
            expected_length_ |= static_cast<std::uint16_t>(byte) << (length_offset_ * 8U);
            if (++length_offset_ < 2) return false;
            if (expected_length_ == 0 ||
                expected_length_ > kUsbUpdateTransportFrameMaximum) {
                ++rejected_frames_;
                reset_parser();
                return false;
            }
            stage_ = Stage::payload;
            payload_offset_ = 0;
            return false;
        case Stage::payload:
            payload_[payload_offset_++] = byte;
            if (payload_offset_ == expected_length_) {
                stage_ = Stage::checksum;
                checksum_offset_ = 0;
                supplied_checksum_ = 0;
            }
            return false;
        case Stage::checksum:
            supplied_checksum_ |= static_cast<std::uint32_t>(byte) <<
                                  (checksum_offset_ * 8U);
            if (++checksum_offset_ < kUsbUpdateEnvelopeChecksumSize) return false;
            if (supplied_checksum_ != usb_update_crc32(
                                         std::span(payload_.data(), expected_length_))) {
                ++rejected_frames_;
                reset_parser();
                return false;
            }
            ready_length_ = expected_length_;
            reset_parser();
            return true;
    }
    reset_parser();
    return false;
}

std::span<const std::uint8_t> UsbUpdateEnvelopeDecoder::payload() const noexcept {
    return std::span(payload_.data(), ready_length_);
}

std::uint32_t UsbUpdateEnvelopeDecoder::rejected_frames() const noexcept {
    return rejected_frames_;
}

bool UsbUpdateEnvelopeDecoder::in_progress() const noexcept {
    return stage_ != Stage::magic || magic_offset_ != 0;
}

void UsbUpdateEnvelopeDecoder::reset() noexcept {
    ready_length_ = 0;
    reset_parser();
}

}  // namespace nightglass::services
