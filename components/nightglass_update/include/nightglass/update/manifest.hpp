#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace nightglass::update {

inline constexpr std::uint32_t kManifestFormatVersion = 1;
inline constexpr std::uint32_t kPartitionRevision = 1;
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kMaximumWriteChunk = 16 * 1024;
inline constexpr char kBoardId[] = "nightglass-waveshare-esp32s3r8-2.06-v1.0";
inline constexpr char kPartitionId[] = "nightglass-32m-r1";

struct UpdateManifest {
    std::uint32_t format_version{kManifestFormatVersion};
    std::array<char, 48> board_id{};
    std::array<char, 32> partition_id{};
    std::uint32_t partition_revision{kPartitionRevision};
    std::array<char, 32> app_version{};
    std::uint32_t secure_version{0};
    std::uint32_t image_size{0};
    std::array<std::uint8_t, kSha256Size> image_sha256{};
};

struct ValidationContext {
    const char *running_version{nullptr};
    std::uint32_t running_secure_version{0};
    std::size_t inactive_slot_size{0};
};

enum class ManifestStatus : std::uint8_t {
    ok,
    malformed,
    unsupported_format,
    wrong_board,
    wrong_partition,
    invalid_size,
    same_version,
    secure_version_downgrade,
    output_too_small,
};

[[nodiscard]] ManifestStatus validate_manifest(const UpdateManifest &manifest,
                                               const ValidationContext &context) noexcept;

// Produces the byte-exact payload that a SignatureVerifier must authenticate.
// The image digest is included, binding the signed metadata to the stream.
[[nodiscard]] ManifestStatus canonical_signature_payload(
    const UpdateManifest &manifest, std::span<char> output,
    std::size_t &written) noexcept;

[[nodiscard]] const char *manifest_status_detail(ManifestStatus status) noexcept;

}  // namespace nightglass::update
