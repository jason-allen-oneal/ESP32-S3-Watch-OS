#pragma once

#include <cstdint>

namespace nightglass::services {

enum class ActivityProcessorState : std::uint8_t {
    warming_up,
    ready,
};

struct ActivitySample {
    float accel_x_g{0.0F};
    float accel_y_g{0.0F};
    float accel_z_g{0.0F};
    std::int64_t sampled_at_us{0};
};

struct ActivityProcessorOutput {
    ActivityProcessorState state{ActivityProcessorState::warming_up};
    bool sample_valid{false};
    bool step_detected{false};
    std::uint16_t warmup_samples{0};
    std::uint16_t warmup_required{0};
    std::uint32_t warmup_restarts{0};
    float acceleration_delta_g{0.0F};
};

// Orientation-independent pedestrian step detector. It learns the local
// gravity magnitude during a short warm-up, then detects band-limited impacts
// with cadence and re-arm gates. It owns no persistence or RTOS state.
class ActivityProcessor {
public:
    static constexpr std::uint16_t kWarmupSamples = 32;

    ActivityProcessorOutput process(const ActivitySample &sample) noexcept;
    void reset() noexcept;

private:
    std::uint16_t warmup_samples_{0};
    std::uint32_t warmup_restarts_{0};
    float gravity_g_{1.0F};
    float filtered_delta_g_{0.0F};
    bool armed_{true};
    std::int64_t last_sample_us_{0};
    std::int64_t last_step_us_{0};
};

}  // namespace nightglass::services
