#include "nightglass/core/health.hpp"

#include <cstring>

#include "esp_timer.h"

namespace nightglass::core {

namespace {
HealthRegistry registry;

template <std::size_t Size>
void copy_text(std::array<char, Size> &destination, const char *source) {
    const char *text = source ? source : "";
    std::strncpy(destination.data(), text, destination.size() - 1);
    destination.back() = '\0';
}
}

bool HealthRegistry::set(const char *component, HealthState state, const char *detail) {
    if (!component || !*component) return false;

    portENTER_CRITICAL(&mux_);
    HealthRecord *slot = nullptr;
    for (auto &record : records_) {
        if (record.occupied && std::strcmp(record.component.data(), component) == 0) {
            slot = &record;
            break;
        }
        if (!slot && !record.occupied) slot = &record;
    }
    if (!slot) {
        portEXIT_CRITICAL(&mux_);
        return false;
    }

    slot->occupied = true;
    copy_text(slot->component, component);
    slot->state = state;
    copy_text(slot->detail, detail);
    if (state == HealthState::ok) {
        slot->last_success_us = esp_timer_get_time();
    } else if (state == HealthState::failed) {
        ++slot->failure_count;
    }
    portEXIT_CRITICAL(&mux_);
    return true;
}

bool HealthRegistry::copy(const char *component, HealthRecord &destination) const {
    if (!component) return false;
    portENTER_CRITICAL(&mux_);
    for (const auto &record : records_) {
        if (record.occupied && std::strcmp(record.component.data(), component) == 0) {
            destination = record;
            portEXIT_CRITICAL(&mux_);
            return true;
        }
    }
    portEXIT_CRITICAL(&mux_);
    return false;
}

std::array<HealthRecord, HealthRegistry::capacity> HealthRegistry::snapshot() const {
    std::array<HealthRecord, capacity> copy{};
    portENTER_CRITICAL(&mux_);
    copy = records_;
    portEXIT_CRITICAL(&mux_);
    return copy;
}

HealthRegistry &health_registry() { return registry; }

}  // namespace nightglass::core
