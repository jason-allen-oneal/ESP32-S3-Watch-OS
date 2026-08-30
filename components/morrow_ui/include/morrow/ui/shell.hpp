#pragma once

#include "morrow/core/status.hpp"

namespace morrow::ui {

class Shell {
public:
    morrow::core::Status start();
};

Shell &shell();

}  // namespace morrow::ui
