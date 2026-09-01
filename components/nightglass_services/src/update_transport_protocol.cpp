#include "nightglass/services/update_transport_protocol.hpp"

#include <algorithm>
#include <cstring>

#include "nightglass/services/connectivity_protocol.hpp"

namespace nightglass::services {
namespace {

std::uint32_t read_u32(const std::uint8_t *data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

void write_u32(std::uint8_t *data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8U);
    data[2] = static_cast<std::uint8_t>(value >> 16U);
    data[3] = static_cast<std::uint8_t>(value >> 24U);
}
std::uint64_t read_u64(const std::uint8_t *data) noexcept {
    return static_cast<std::uint64_t>(read_u32(data)) |
           (static_cast<std::uint64_t>(read_u32(data + 4)) << 32U);
}
void write_u64(std::uint8_t *data, std::uint64_t value) noexcept {
    write_u32(data, static_cast<std::uint32_t>(value));
    write_u32(data + 4, static_cast<std::uint32_t>(value >> 32U));
}

template <std::size_t N>
bool copy_canonical(std::array<char, N> &output, const std::uint8_t *input,
                    std::size_t length) noexcept {
    if (length == 0 || length >= N) return false;
    if (!std::all_of(input, input + length, [](std::uint8_t value) {
            return value >= 0x21 && value <= 0x7e && value != '\\' && value != '=';
        })) return false;
    std::copy_n(input, length, output.begin());
    output[length] = '\0';
    return true;
}

}  // namespace

bool is_update_transport_frame(std::span<const std::uint8_t> frame) noexcept {
    if (frame.size() < 2 || frame[0] != kCompanionProtocolVersion) return false;
    return frame[1] >= kUpdateBeginOpcode && frame[1] <= kUpdateStatusQueryOpcode;
}

bool parse_update_transport_frame(std::span<const std::uint8_t> frame,
                                  UpdateTransportCommand &command) noexcept {
    command = {};
    if (!is_update_transport_frame(frame) || frame.size() > kUpdateTransportFrameMaximum) {
        return false;
    }
    if (frame.size() < 10) return false;
    command.session = read_u64(frame.data() + 2);
    if (command.session == 0) return false;
    switch (frame[1]) {
        case kUpdateBeginOpcode: {
            constexpr std::size_t kFixed = 62;
            if (frame.size() < kFixed) return false;
            const std::size_t board_size = frame[58];
            const std::size_t partition_size = frame[59];
            const std::size_t version_size = frame[60];
            const std::size_t signature_size = frame[61];
            if (signature_size < 8 || signature_size > kUpdateSignatureMaximum ||
                frame.size() != kFixed + board_size + partition_size + version_size +
                                    signature_size) return false;
            command.manifest.format_version = read_u32(frame.data() + 10);
            command.manifest.partition_revision = read_u32(frame.data() + 14);
            command.manifest.secure_version = read_u32(frame.data() + 18);
            command.manifest.image_size = read_u32(frame.data() + 22);
            std::copy_n(frame.begin() + 26, command.manifest.image_sha256.size(),
                        command.manifest.image_sha256.begin());
            std::size_t offset = kFixed;
            if (!copy_canonical(command.manifest.board_id, frame.data() + offset,
                                board_size)) return false;
            offset += board_size;
            if (!copy_canonical(command.manifest.partition_id, frame.data() + offset,
                                partition_size)) return false;
            offset += partition_size;
            if (!copy_canonical(command.manifest.app_version, frame.data() + offset,
                                version_size)) return false;
            offset += version_size;
            command.signature_size = static_cast<std::uint8_t>(signature_size);
            std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), signature_size,
                        command.signature.begin());
            command.kind = UpdateTransportCommandKind::begin;
            return true;
        }
        case kUpdateDataOpcode:
            if (frame.size() <= 14 || frame.size() > kUpdateTransportFrameMaximum) return false;
            command.offset = read_u32(frame.data() + 10);
            command.data_size = static_cast<std::uint8_t>(frame.size() - 14);
            std::copy(frame.begin() + 14, frame.end(), command.data.begin());
            command.kind = UpdateTransportCommandKind::data;
            return true;
        case kUpdateFinishOpcode:
            if (frame.size() != 10) return false;
            command.kind = UpdateTransportCommandKind::finish;
            return true;
        case kUpdateAbortOpcode:
            if (frame.size() != 10) return false;
            command.kind = UpdateTransportCommandKind::abort;
            return true;
        case kUpdateStatusQueryOpcode:
            if (frame.size() != 10) return false;
            command.kind = UpdateTransportCommandKind::status;
            return true;
        default:
            return false;
    }
}

std::array<std::uint8_t, 22> encode_update_transport_status(
    const UpdateTransportStatus &status) noexcept {
    std::array<std::uint8_t, 22> frame{};
    frame[0] = kCompanionProtocolVersion;
    frame[1] = kUpdateStatusOpcode;
    write_u64(frame.data() + 2, status.session);
    frame[10] = status.state;
    frame[11] = status.signature_state;
    frame[12] = status.result;
    frame[13] = 0;
    write_u32(frame.data() + 14, status.expected_bytes);
    write_u32(frame.data() + 18, status.received_bytes);
    return frame;
}

bool update_stream_position_matches(std::uint64_t active_session,
                                    std::uint64_t candidate_session,
                                    std::uint32_t received_bytes,
                                    std::uint32_t candidate_offset,
                                    bool receiving,
                                    bool awaiting_confirmation) noexcept {
    return active_session != 0 && active_session == candidate_session && receiving &&
           !awaiting_confirmation && received_bytes == candidate_offset;
}

}  // namespace nightglass::services
