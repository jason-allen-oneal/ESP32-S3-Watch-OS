#pragma once

#include "morrow/core/status.hpp"

namespace morrow::core {

enum class PowerState { active, dim, ambient, display_off, light_sleep, deep_sleep };
enum class WakeReason { cold_boot, button, touch, motion, alarm, timer, usb, unknown };

class IService {
public:
    virtual ~IService() = default;
    virtual Status probe() = 0;
    virtual Status start() = 0;
    virtual void suspend(PowerState target) = 0;
    virtual void resume(WakeReason reason) = 0;
    virtual void stop() = 0;
};

}  // namespace morrow::core
