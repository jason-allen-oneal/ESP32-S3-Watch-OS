#pragma once

#include <cstdint>

#include "nightglass/core/status.hpp"

namespace nightglass::services {

struct UsbUpdateSnapshot {
    bool started{false};
    bool connected{false};
    std::uint32_t received_frames{0};
    std::uint32_t rejected_frames{0};
};

class UsbUpdateService {
public:
    // Starts a signed, application-only update endpoint on the watch's native
    // USB Serial/JTAG link. The endpoint never enters the ROM flasher and all
    // writes continue through UpdateService's inactive-slot/rollback path.
    nightglass::core::Status start();
    [[nodiscard]] UsbUpdateSnapshot snapshot() const noexcept;
};

UsbUpdateService &usb_update_service();

}  // namespace nightglass::services
