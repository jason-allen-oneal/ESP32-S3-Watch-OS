#include <cassert>
#include <cmath>
#include <limits>

#include "nightglass/services/gyro_processor.hpp"

using nightglass::services::GyroProcessor;
using nightglass::services::GyroSample;

namespace {

bool near(float actual, float expected, float tolerance = 0.02F) {
    return std::fabs(actual - expected) <= tolerance;
}

GyroSample stationary(float x, float y, float z) {
    return {.x_dps = x, .y_dps = y, .z_dps = z, .accel_z_g = 1.0F};
}

void warm_up(GyroProcessor &processor) {
    for (int i = 0; i < GyroProcessor::kWarmupSamples; ++i) {
        const auto output = processor.process(stationary(100.0F, 0.0F, 0.0F));
        assert(!output.calibrated);
        assert(output.calibration_samples == 0);
    }
}

}  // namespace

int main() {
    GyroProcessor processor;
    nightglass::services::GyroOutput output{};

    warm_up(processor);
    for (int i = 0; i < GyroProcessor::kRequiredSamples; ++i) {
        const float noise = i % 2 == 0 ? 0.1F : -0.1F;
        output = processor.process(stationary(2.0F + noise, -1.0F - noise, 0.25F + noise));
    }
    assert(output.calibrated);
    assert(output.calibration_samples == GyroProcessor::kRequiredSamples);
    assert(near(output.bias_x_dps, 2.0F));
    assert(near(output.bias_y_dps, -1.0F));
    assert(near(output.bias_z_dps, 0.25F));
    assert(output.display_x_dps == 0.0F);

    output = processor.process(stationary(3.0F, -1.0F, 0.25F));
    assert(near(output.filtered_x_dps, GyroProcessor::kFilterAlpha));
    assert(output.display_x_dps == 0.0F);
    for (int i = 0; i < 12; ++i) output = processor.process(stationary(3.0F, -1.0F, 0.25F));
    assert(output.filtered_x_dps > GyroProcessor::kDisplayDeadbandDps);
    assert(output.display_x_dps > GyroProcessor::kDisplayDeadbandDps);

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < GyroProcessor::kRequiredSamples - 1; ++i) {
        output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    }
    output = processor.process(stationary(4.0F, 0.0F, 0.0F));
    assert(!output.calibrated);
    assert(output.calibration_restarted);
    assert(output.calibration_samples == 0);
    assert(output.calibration_restarts == 1);
    for (int i = 0; i < GyroProcessor::kRequiredSamples; ++i) {
        output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    }
    assert(output.calibrated);
    assert(near(output.bias_x_dps, 2.0F));

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < 10; ++i) output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    auto accelerated = stationary(2.0F, 0.0F, 0.0F);
    accelerated.accel_z_g = 1.2F;
    output = processor.process(accelerated);
    assert(output.calibration_restarted);
    assert(output.calibration_samples == 0);

    processor.reset();
    warm_up(processor);
    output = processor.process(stationary(7.0F, 0.0F, 0.0F));
    assert(!output.calibrated);
    assert(output.calibration_samples == 0);

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < GyroProcessor::kRequiredSamples; ++i) {
        const float spread = i % 2 == 0 ? 1.0F : -1.0F;
        output = processor.process(stationary(2.0F + spread, 0.0F, 0.0F));
    }
    assert(!output.calibrated);
    assert(output.calibration_restarted);
    assert(output.calibration_samples == 0);

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < 25; ++i) output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    const auto before_invalid = output.calibration_samples;
    output = processor.process(
        stationary(std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F));
    assert(!output.calibrated);
    assert(output.calibration_samples == before_invalid);

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < GyroProcessor::kRequiredSamples; ++i) {
        output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    }
    float previous = 0.0F;
    for (int i = 0; i < 20; ++i) {
        output = processor.process(stationary(4.0F, 0.0F, 0.0F));
        assert(output.filtered_x_dps >= previous);
        assert(output.filtered_x_dps <= 2.0F);
        previous = output.filtered_x_dps;
    }

    processor.reset();
    warm_up(processor);
    for (int i = 0; i < GyroProcessor::kRequiredSamples; ++i) {
        output = processor.process(stationary(2.0F, 0.0F, 0.0F));
    }
    output = processor.process(stationary(2.1F, 0.0F, 0.0F));
    assert(output.display_x_dps == 0.0F);
    assert(!std::signbit(output.display_x_dps));
    output = processor.process(stationary(1.9F, 0.0F, 0.0F));
    assert(output.display_x_dps == 0.0F);
    assert(!std::signbit(output.display_x_dps));
}
