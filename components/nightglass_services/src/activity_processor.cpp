#include "nightglass/services/activity_processor.hpp"

#include <cmath>

namespace nightglass::services {
namespace {

constexpr float kGravityAlpha = 0.08F;
constexpr float kSignalAlpha = 0.35F;
constexpr float kStepThresholdG = 0.09F;
constexpr float kRearmThresholdG = 0.025F;
constexpr std::int64_t kMinimumStepIntervalUs = 280'000;
constexpr std::int64_t kMaximumSampleGapUs = 250'000;

bool finite_sample(const ActivitySample &sample) noexcept {
    return std::isfinite(sample.accel_x_g) && std::isfinite(sample.accel_y_g) &&
           std::isfinite(sample.accel_z_g) && sample.sampled_at_us > 0;
}

}  // namespace

ActivityProcessorOutput ActivityProcessor::process(const ActivitySample &sample) noexcept {
    ActivityProcessorOutput output{};
    output.warmup_required = kWarmupSamples;
    output.warmup_samples = warmup_samples_;
    output.warmup_restarts = warmup_restarts_;
    if (!finite_sample(sample) || sample.sampled_at_us <= last_sample_us_) return output;

    const float magnitude = std::sqrt(sample.accel_x_g * sample.accel_x_g +
                                      sample.accel_y_g * sample.accel_y_g +
                                      sample.accel_z_g * sample.accel_z_g);
    if (!std::isfinite(magnitude) || magnitude < 0.2F || magnitude > 3.5F) return output;

    if (last_sample_us_ > 0 && sample.sampled_at_us - last_sample_us_ > kMaximumSampleGapUs) {
        filtered_delta_g_ = 0.0F;
        armed_ = true;
    }
    last_sample_us_ = sample.sampled_at_us;
    output.sample_valid = true;

    if (warmup_samples_ < kWarmupSamples) {
        if ((magnitude < 0.75F || magnitude > 1.25F) ||
            (warmup_samples_ > 0 && std::fabs(magnitude - gravity_g_) > 0.08F)) {
            warmup_samples_ = 0;
            ++warmup_restarts_;
            gravity_g_ = magnitude;
            filtered_delta_g_ = 0.0F;
            output.warmup_samples = 0;
            output.warmup_restarts = warmup_restarts_;
            return output;
        }
        gravity_g_ += (magnitude - gravity_g_) /
                      static_cast<float>(warmup_samples_ + 1U);
        ++warmup_samples_;
        output.warmup_samples = warmup_samples_;
        output.warmup_restarts = warmup_restarts_;
        output.state = warmup_samples_ == kWarmupSamples
                           ? ActivityProcessorState::ready
                           : ActivityProcessorState::warming_up;
        return output;
    }

    gravity_g_ += kGravityAlpha * (magnitude - gravity_g_);
    const float delta = magnitude - gravity_g_;
    filtered_delta_g_ += kSignalAlpha * (delta - filtered_delta_g_);
    output.state = ActivityProcessorState::ready;
    output.acceleration_delta_g = filtered_delta_g_;

    if (!armed_ && filtered_delta_g_ <= kRearmThresholdG) armed_ = true;
    if (armed_ && filtered_delta_g_ >= kStepThresholdG &&
        (last_step_us_ == 0 || sample.sampled_at_us - last_step_us_ >= kMinimumStepIntervalUs)) {
        output.step_detected = true;
        armed_ = false;
        last_step_us_ = sample.sampled_at_us;
    }
    return output;
}

void ActivityProcessor::reset() noexcept { *this = {}; }

}  // namespace nightglass::services
