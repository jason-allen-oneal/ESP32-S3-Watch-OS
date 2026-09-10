#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "nightglass/core/status.hpp"
#include "nightglass/services/update_transport_protocol.hpp"

namespace nightglass::services {

using UpdateStatusSink = bool (*)(const std::uint8_t *, std::size_t);

struct UpdateTransportSnapshot {
    std::uint32_t sequence{0};
    std::uint64_t session{0};
    UpdateTransportLink link{UpdateTransportLink::none};
    bool awaiting_confirmation{false};
    bool ready_to_reboot{false};
    std::array<char, 32> target_version{};
};

class UpdateTransport {
public:
    nightglass::core::Status start(UpdateTransportLink link, UpdateStatusSink sink);
    [[nodiscard]] bool enqueue(const UpdateTransportCommand &command) noexcept;
    [[nodiscard]] bool abort() noexcept;
    void link_ready(UpdateTransportLink link) noexcept;
    void link_lost(UpdateTransportLink link) noexcept;
    [[nodiscard]] UpdateTransportSnapshot snapshot() const noexcept;
};

UpdateTransport &update_transport();

}  // namespace nightglass::services
