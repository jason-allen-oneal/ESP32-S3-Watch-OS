#pragma once

#include <cstdint>

namespace morrow::core {

enum class StatusCode : std::uint8_t {
    ok,
    degraded,
    unavailable,
    invalid_state,
    timeout,
    io_error,
    no_memory,
};

struct Status {
    StatusCode code{StatusCode::ok};
    const char *detail{"ok"};

    [[nodiscard]] constexpr bool is_ok() const { return code == StatusCode::ok; }
    [[nodiscard]] static constexpr Status Ok() { return {}; }
};

}  // namespace morrow::core
