#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "nightglass/update/manifest.hpp"

namespace nightglass::services {

inline constexpr std::uint8_t kUpdateBeginOpcode = 0x30;
inline constexpr std::uint8_t kUpdateDataOpcode = 0x31;
inline constexpr std::uint8_t kUpdateFinishOpcode = 0x32;
inline constexpr std::uint8_t kUpdateAbortOpcode = 0x33;
inline constexpr std::uint8_t kUpdateStatusQueryOpcode = 0x34;
inline constexpr std::uint8_t kUpdateStatusOpcode = 0x35;
inline constexpr std::size_t kUpdateTransportFrameMaximum = 244;
inline constexpr std::size_t kUpdateSignatureMaximum = 72;
inline constexpr std::size_t kUpdateDataMaximum = kUpdateTransportFrameMaximum - 14;

enum class UpdateTransportCommandKind : std::uint8_t {
    invalid,
    begin,
    data,
    finish,
    abort,
    status,
    internal_confirm,
};

struct UpdateTransportCommand {
    UpdateTransportCommandKind kind{UpdateTransportCommandKind::invalid};
    std::uint64_t session{0};
    std::uint32_t offset{0};
    nightglass::update::UpdateManifest manifest{};
    std::array<std::uint8_t, kUpdateSignatureMaximum> signature{};
    std::uint8_t signature_size{0};
    std::array<std::uint8_t, kUpdateDataMaximum> data{};
    std::uint8_t data_size{0};
};

struct UpdateTransportStatus {
    std::uint64_t session{0};
    std::uint8_t state{0};
    std::uint8_t signature_state{0};
    std::uint8_t result{0};
    std::uint32_t expected_bytes{0};
    std::uint32_t received_bytes{0};
};

[[nodiscard]] bool is_update_transport_frame(std::span<const std::uint8_t> frame) noexcept;
[[nodiscard]] bool parse_update_transport_frame(std::span<const std::uint8_t> frame,
                                                UpdateTransportCommand &command) noexcept;
[[nodiscard]] std::array<std::uint8_t, 22> encode_update_transport_status(
    const UpdateTransportStatus &status) noexcept;
[[nodiscard]] bool update_stream_position_matches(
    std::uint64_t active_session, std::uint64_t candidate_session,
    std::uint32_t received_bytes, std::uint32_t candidate_offset,
    bool receiving, bool awaiting_confirmation) noexcept;

}  // namespace nightglass::services
