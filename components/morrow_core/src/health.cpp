#include "morrow/core/health.hpp"

#include <cstring>

#include "esp_timer.h"

namespace morrow::core {

namespace {
HealthRegistry registry;
}

bool HealthRegistry::set(const char *component, HealthState state, const char *detail) {
    if (!component || !*component) return false;

    HealthRecord *slot = nullptr;
    for (auto &record : records_) {
        if (record.component && std::strcmp(record.component, component) == 0) {
            slot = &record;
            break;
        }
        if (!slot && !record.component) slot = &record;
    }
    if (!slot) return false;

    slot->component = component;
    slot->state = state;
    slot->detail = detail ? detail : "";
    if (state == HealthState::ok) {
        slot->last_success_us = esp_timer_get_time();
    } else if (state == HealthState::failed) {
        ++slot->failure_count;
    }
    return true;
}

const HealthRecord *HealthRegistry::find(const char *component) const {
    if (!component) return nullptr;
    for (const auto &record : records_) {
        if (record.component && std::strcmp(record.component, component) == 0) return &record;
    }
    return nullptr;
}

HealthRegistry &health_registry() { return registry; }

}  // namespace morrow::core
