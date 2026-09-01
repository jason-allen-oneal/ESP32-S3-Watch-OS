#include "nightglass/update/manifest.hpp"
#include "nightglass/update/policy.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

template <std::size_t Size>
void copy_text(std::array<char, Size> &destination, const char *source) {
    std::strncpy(destination.data(), source, destination.size() - 1);
}

nightglass::update::UpdateManifest valid_manifest() {
    nightglass::update::UpdateManifest manifest{};
    copy_text(manifest.board_id, nightglass::update::kBoardId);
    copy_text(manifest.partition_id, nightglass::update::kPartitionId);
    copy_text(manifest.app_version, "v0.3.0");
    manifest.secure_version = 2;
    manifest.image_size = 1024;
    std::fill(manifest.image_sha256.begin(), manifest.image_sha256.end(), 0xAB);
    return manifest;
}

}  // namespace

int main(int argc, char **argv) {
    using namespace nightglass::update;
    const ValidationContext context{
        .running_version = "v0.2.0",
        .running_secure_version = 1,
        .inactive_slot_size = 6 * 1024 * 1024,
    };

    auto manifest = valid_manifest();
    assert(validate_manifest(manifest, context) == ManifestStatus::ok);

    manifest.image_size = static_cast<std::uint32_t>(context.inactive_slot_size + 1);
    assert(validate_manifest(manifest, context) == ManifestStatus::invalid_size);
    manifest = valid_manifest();
    copy_text(manifest.board_id, "another-board");
    assert(validate_manifest(manifest, context) == ManifestStatus::wrong_board);
    manifest = valid_manifest();
    copy_text(manifest.app_version, "v0.2.0");
    assert(validate_manifest(manifest, context) == ManifestStatus::same_version);
    manifest = valid_manifest();
    manifest.secure_version = 0;
    assert(validate_manifest(manifest, context) == ManifestStatus::secure_version_downgrade);
    manifest = valid_manifest();
    manifest.image_sha256 = {};
    assert(validate_manifest(manifest, context) == ManifestStatus::malformed);
    manifest = valid_manifest();
    manifest.app_version[7] = 'x';
    assert(validate_manifest(manifest, context) == ManifestStatus::malformed);
    manifest = valid_manifest();
    copy_text(manifest.app_version, "v0.3.0\nforged=1");
    assert(validate_manifest(manifest, context) == ManifestStatus::malformed);
    manifest = valid_manifest();
    copy_text(manifest.app_version, "v0.3.0=forged");
    assert(validate_manifest(manifest, context) == ManifestStatus::malformed);

    manifest = valid_manifest();
    std::array<char, 512> payload{};
    std::size_t payload_size = 0;
    assert(canonical_signature_payload(manifest, payload, payload_size) == ManifestStatus::ok);
    const std::string_view payload_view(payload.data(), payload_size);
    assert(payload_view.starts_with("NIGHTGLASS-OTA-MANIFEST\nformat=1\n"));
    assert(payload_view.find("board=nightglass-waveshare-esp32s3r8-2.06-v1.0\n") !=
           std::string_view::npos);
    assert(payload_view.find("size=1024\n") != std::string_view::npos);
    assert(payload_view.ends_with(
        "sha256=abababababababababababababababababababababababababababababababab\n"));
    if (argc == 2 && std::string_view(argv[1]) == "--print-payload") {
        std::cout.write(payload.data(), static_cast<std::streamsize>(payload_size));
        return 0;
    }
    std::array<char, 16> too_small{};
    assert(canonical_signature_payload(manifest, too_small, payload_size) ==
           ManifestStatus::output_too_small);

    auto boot = evaluate_boot(1, false, false);
    assert(!boot.safe_mode);
    assert(evaluate_health_gate(boot, true) == HealthGateAction::record_healthy);
    assert(evaluate_health_gate(boot, false) == HealthGateAction::retain_unhealthy);

    boot = evaluate_boot(kUnhealthyBootLimit, false, false);
    assert(boot.safe_mode);
    boot = evaluate_boot(1, true, false);
    assert(boot.safe_mode);
    boot = evaluate_boot(1, false, true);
    assert(evaluate_health_gate(boot, true) == HealthGateAction::accept_pending_image);
    assert(evaluate_health_gate(boot, false) == HealthGateAction::rollback_pending_image);
    boot = evaluate_boot(1, true, true);
    assert(evaluate_health_gate(boot, true) == HealthGateAction::rollback_pending_image);
}
