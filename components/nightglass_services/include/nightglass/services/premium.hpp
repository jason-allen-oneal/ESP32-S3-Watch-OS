#pragma once
#include "nightglass/services/premium_protocol.hpp"

namespace nightglass::services {
class PremiumService {
public:
    void start();
    PremiumProfile profile() const;
    bool save(const PremiumProfile &profile);
    bool accept(std::span<const std::uint8_t> frame);
    bool request_content(std::uint8_t kind, std::uint32_t target = 0);
    std::size_t copy_content(std::span<std::uint8_t> destination,
                             std::uint32_t &token, std::uint8_t &kind,
                             std::uint32_t &target) const;
    bool action(std::uint8_t action, std::uint8_t index);
    void clear_content();
    std::uint8_t action_status() const;
};
PremiumService &premium_service();
}  // namespace nightglass::services
