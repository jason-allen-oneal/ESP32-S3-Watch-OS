#include "nightglass/update/manifest.hpp"

#include <cstdio>
#include <cstring>

namespace nightglass::update {
namespace {

template <std::size_t Size>
bool canonical_text(const std::array<char, Size> &value) noexcept {
    if (value[0] == '\0') return false;
    bool terminated = false;
    for (const char character : value) {
        if (terminated) {
            if (character != '\0') return false;
            continue;
        }
        if (character == '\0') {
            terminated = true;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21 || byte > 0x7E || character == '\\' || character == '=') {
            return false;
        }
    }
    return terminated;
}

bool digest_is_nonzero(const std::array<std::uint8_t, kSha256Size> &digest) noexcept {
    std::uint8_t combined = 0;
    for (const auto byte : digest) combined |= byte;
    return combined != 0;
}

}  // namespace

ManifestStatus validate_manifest(const UpdateManifest &manifest,
                                 const ValidationContext &context) noexcept {
    if (!canonical_text(manifest.board_id) ||
        !canonical_text(manifest.partition_id) ||
        !canonical_text(manifest.app_version) || !digest_is_nonzero(manifest.image_sha256)) {
        return ManifestStatus::malformed;
    }
    if (manifest.format_version != kManifestFormatVersion) {
        return ManifestStatus::unsupported_format;
    }
    if (std::strcmp(manifest.board_id.data(), kBoardId) != 0) {
        return ManifestStatus::wrong_board;
    }
    if (manifest.partition_revision != kPartitionRevision ||
        std::strcmp(manifest.partition_id.data(), kPartitionId) != 0) {
        return ManifestStatus::wrong_partition;
    }
    if (manifest.image_size == 0 || manifest.image_size > context.inactive_slot_size) {
        return ManifestStatus::invalid_size;
    }
    if (context.running_version != nullptr &&
        std::strcmp(manifest.app_version.data(), context.running_version) == 0) {
        return ManifestStatus::same_version;
    }
    if (manifest.secure_version <= context.running_secure_version) {
        return ManifestStatus::secure_version_downgrade;
    }
    return ManifestStatus::ok;
}

ManifestStatus canonical_signature_payload(const UpdateManifest &manifest,
                                           std::span<char> output,
                                           std::size_t &written) noexcept {
    written = 0;
    if (!canonical_text(manifest.board_id) ||
        !canonical_text(manifest.partition_id) ||
        !canonical_text(manifest.app_version)) {
        return ManifestStatus::malformed;
    }

    char digest_hex[(kSha256Size * 2) + 1]{};
    for (std::size_t index = 0; index < manifest.image_sha256.size(); ++index) {
        std::snprintf(digest_hex + (index * 2), 3, "%02x", manifest.image_sha256[index]);
    }
    const int result = std::snprintf(
        output.data(), output.size(),
        "NIGHTGLASS-OTA-MANIFEST\nformat=%lu\nboard=%s\npartition=%s\n"
        "partition_revision=%lu\nversion=%s\nsecure_version=%lu\nsize=%lu\nsha256=%s\n",
        static_cast<unsigned long>(manifest.format_version), manifest.board_id.data(),
        manifest.partition_id.data(), static_cast<unsigned long>(manifest.partition_revision),
        manifest.app_version.data(), static_cast<unsigned long>(manifest.secure_version),
        static_cast<unsigned long>(manifest.image_size), digest_hex);
    if (result < 0 || static_cast<std::size_t>(result) >= output.size()) {
        return ManifestStatus::output_too_small;
    }
    written = static_cast<std::size_t>(result);
    return ManifestStatus::ok;
}

const char *manifest_status_detail(ManifestStatus status) noexcept {
    switch (status) {
        case ManifestStatus::ok: return "manifest valid";
        case ManifestStatus::malformed: return "manifest fields malformed";
        case ManifestStatus::unsupported_format: return "manifest format unsupported";
        case ManifestStatus::wrong_board: return "manifest targets another board";
        case ManifestStatus::wrong_partition: return "partition revision mismatch";
        case ManifestStatus::invalid_size: return "image size outside inactive slot";
        case ManifestStatus::same_version: return "image version is already running";
        case ManifestStatus::secure_version_downgrade: return "secure version downgrade refused";
        case ManifestStatus::output_too_small: return "canonical manifest buffer too small";
    }
    return "unknown manifest error";
}

}  // namespace nightglass::update
