#pragma once

#include "nightglass/core/status.hpp"

namespace nightglass::core {

enum class PowerState { active, dim, ambient, screen_blank, light_sleep, deep_sleep };
enum class WakeReason {
    cold_boot, button, touch, motion, notification, alarm, timer, usb, unknown
};

class IService {
public:
    virtual ~IService() = default;
    virtual Status probe() = 0;
    virtual Status start() = 0;
    virtual void suspend(PowerState target) = 0;
    virtual void resume(WakeReason reason) = 0;
    virtual void stop() = 0;
};

}  // namespace nightglass::core
