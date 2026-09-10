#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

#include "nightglass/services/activity_processor.hpp"

using nightglass::services::ActivityProcessor;
using nightglass::services::ActivityProcessorState;
using nightglass::services::ActivitySample;

namespace {

std::int64_t timestamp = 0;

auto sample(ActivityProcessor &processor, float magnitude_g, std::int64_t advance_us = 40'000) {
    timestamp += advance_us;
    return processor.process(ActivitySample{0.0F, 0.0F, magnitude_g, timestamp});
}

void warm_up(ActivityProcessor &processor) {
    for (std::uint16_t index = 0; index < ActivityProcessor::kWarmupSamples; ++index) {
        const auto output = sample(processor, 1.0F);
        assert(output.sample_valid);
    }
}

}  // namespace

int main() {
    timestamp = 0;
    ActivityProcessor processor;
    for (std::uint16_t index = 0; index + 1 < ActivityProcessor::kWarmupSamples; ++index) {
        const auto output = sample(processor, 1.0F);
        assert(output.state == ActivityProcessorState::warming_up);
        assert(!output.step_detected);
    }
    const auto ready = sample(processor, 1.0F);
    assert(ready.state == ActivityProcessorState::ready);
    assert(!ready.step_detected);

    int steps = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (int frame = 0; frame < 8; ++frame) {
            const float values[]{1.0F, 1.14F, 1.34F, 1.18F, 0.96F, 0.88F, 0.98F, 1.0F};
            if (sample(processor, values[frame]).step_detected) ++steps;
        }
    }
    assert(steps >= 6 && steps <= 8);

    // Blanked-mode hardware polling runs at 10 Hz. Preserve one detected step
    // per walking waveform at that cadence so power throttling cannot silently
    // disable the accelerometer-only activity path.
    timestamp = 0;
    ActivityProcessor blanked;
    for (std::uint16_t index = 0; index < ActivityProcessor::kWarmupSamples; ++index) {
        sample(blanked, 1.0F, 100'000);
    }
    int blanked_steps = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (int frame = 0; frame < 8; ++frame) {
            const float values[]{1.0F, 1.14F, 1.34F, 1.18F, 0.96F, 0.88F, 0.98F, 1.0F};
            if (sample(blanked, values[frame], 100'000).step_detected) ++blanked_steps;
        }
    }
    assert(blanked_steps >= 6 && blanked_steps <= 8);

    timestamp = 0;
    ActivityProcessor quiet;
    warm_up(quiet);
    for (int index = 0; index < 500; ++index) {
        const float noise = static_cast<float>((index % 7) - 3) * 0.003F;
        assert(!sample(quiet, 1.0F + noise).step_detected);
    }

    const auto duplicate = quiet.process({0.0F, 0.0F, 1.4F, timestamp});
    assert(!duplicate.sample_valid);
    const auto invalid = quiet.process({0.0F, 0.0F,
                                        std::numeric_limits<float>::quiet_NaN(),
                                        timestamp + 40'000});
    assert(!invalid.sample_valid);

    timestamp = 0;
    ActivityProcessor moved_during_warmup;
    for (int index = 0; index < 10; ++index) sample(moved_during_warmup, 1.0F);
    const auto restarted = sample(moved_during_warmup, 1.35F);
    assert(restarted.state == ActivityProcessorState::warming_up);
    assert(restarted.warmup_samples == 0);
    assert(restarted.warmup_restarts == 1);

    timestamp = 0;
    ActivityProcessor isolated;
    warm_up(isolated);
    int isolated_steps = 0;
    for (int frame = 0; frame < 8; ++frame) {
        const float values[]{1.0F, 1.14F, 1.34F, 1.18F, 0.96F, 0.88F, 0.98F, 1.0F};
        if (sample(isolated, values[frame]).step_detected) ++isolated_steps;
    }
    assert(isolated_steps == 1);
    return 0;
}
