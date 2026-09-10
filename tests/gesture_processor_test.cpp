#include <cassert>
#include <cstdint>
#include <limits>

#include "nightglass/services/gesture_processor.hpp"

using nightglass::services::GestureKind;
using nightglass::services::GestureProcessor;
using nightglass::services::GestureSample;

namespace {

std::int64_t now_us = 0;

GestureSample sample(float az = 1.0F, float gx = 0.0F, float gy = 0.0F,
                     float ax = 0.0F, bool allow_raise = false,
                     bool gyro_calibrated = true) {
    now_us += 40'000;
    return {.accel_x_g = ax,
            .accel_y_g = 0.0F,
            .accel_z_g = az,
            .gyro_x_dps = gx,
            .gyro_y_dps = gy,
            .gyro_z_dps = 0.0F,
            .sampled_at_us = now_us,
            .allow_raise = allow_raise,
            .gyro_calibrated = gyro_calibrated};
}

void settle(GestureProcessor &processor, int count = 35) {
    for (int index = 0; index < count; ++index) processor.process(sample());
}

}  // namespace

int main() {
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 8; ++index) {
            processor.process(sample(0.2F, 0.0F, 0.0F, 0.98F, true, false));
        }
        processor.process(sample(-0.45F, 45.0F, 0.0F, 0.85F, true, false));
        processor.process(sample(-0.55F, 45.0F, 0.0F, 0.80F, true, false));
        auto output = processor.process(sample(-0.82F, 10.0F, 0.0F, 0.52F, true, false));
        assert(output.detected == GestureKind::none);
        output = processor.process(sample(-0.86F, 4.0F, 0.0F, 0.45F, true, false));
        assert(output.detected == GestureKind::none);
        output = processor.process(sample(-0.88F, 3.0F, 0.0F, 0.42F, true, false));
        assert(output.detected == GestureKind::raise);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 8; ++index) {
            processor.process(sample(-0.45F, 0.0F, 0.0F, 0.89F, true, false));
        }
        // Ordinary wrist raises can pause for more than the old 1.2-second
        // window after arming; the learned finish orientation still gates it.
        for (int index = 0; index < 28; ++index) {
            processor.process(sample(-0.45F, 0.0F, 0.0F, 0.89F, true, false));
        }
        processor.process(sample(-0.55F, 45.0F, 0.0F, 0.83F, true, false));
        processor.process(sample(-0.65F, 45.0F, 0.0F, 0.76F, true, false));
        processor.process(sample(-0.78F, 10.0F, 0.0F, 0.62F, true, false));
        processor.process(sample(-0.80F, 4.0F, 0.0F, 0.60F, true, false));
        const auto output =
            processor.process(sample(-0.82F, 3.0F, 0.0F, 0.57F, true, false));
        assert(output.detected == GestureKind::raise);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        processor.process(sample(1.0F, 120.0F));
        processor.process(sample(1.0F, 125.0F));
        processor.process(sample(1.0F, 15.0F));
        processor.process(sample(1.0F, 12.0F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, -105.0F));
        processor.process(sample(1.0F, -110.0F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, 8.0F));
        auto output = processor.process(sample(1.0F, 6.0F));
        assert(output.detected == GestureKind::double_twist);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        processor.process(sample(1.0F, 190.0F, 0.0F, 0.70F));
        processor.process(sample(1.0F, 195.0F, 0.0F, 0.70F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, -75.0F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, 8.0F));
        auto output = processor.process(sample(1.0F, 6.0F));
        assert(output.detected == GestureKind::flick);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        processor.process(sample(1.0F, 200.0F, 0.0F, 0.70F));
        processor.process(sample(1.0F, 205.0F, 0.0F, 0.70F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, -120.0F));
        processor.process(sample(1.0F, -125.0F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, 8.0F));
        const auto output = processor.process(sample(1.0F, 6.0F));
        assert(output.detected == GestureKind::flick);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        processor.process(sample(1.0F, 80.0F, 0.0F, 1.40F));
        processor.process(sample());
        processor.process(sample(1.0F, -80.0F, 0.0F, -1.40F));
        processor.process(sample());
        processor.process(sample(1.0F, 80.0F, 0.0F, 1.45F));
        processor.process(sample());
        auto output = processor.process(sample(1.0F, -80.0F, 0.0F, -1.40F));
        assert(output.detected == GestureKind::shake);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        settle(processor);
        for (int index = 0; index < 20; ++index) {
            const auto output = processor.process(sample(1.0F, 8.0F, 6.0F, 0.08F));
            assert(output.detected == GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        GestureSample duplicate = sample();
        assert(processor.process(duplicate).sample_valid);
        assert(!processor.process(duplicate).sample_valid);
        GestureSample gap = sample();
        gap.sampled_at_us += 300'000;
        now_us = gap.sampled_at_us;
        assert(processor.process(gap).detected == GestureKind::none);
    }
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 8; ++index) {
            processor.process(sample(0.2F, 0.0F, 0.0F, 0.98F, false));
        }
        processor.process(sample(-0.45F, 50.0F, 0.0F, 0.85F, false));
        processor.process(sample(-0.55F, 50.0F, 0.0F, 0.80F, false));
        for (int index = 0; index < 4; ++index) {
            assert(processor.process(sample(-0.85F, 4.0F, 0.0F, 0.45F, false)).detected ==
                   GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 60; ++index) {
            assert(processor.process(sample(-0.80F, 0.0F, 0.0F, 0.60F, true, false))
                       .detected == GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 6; ++index) {
            const auto peak = processor.process(sample(1.0F, 80.0F, 0.0F, 1.40F));
            assert(peak.detected == GestureKind::none);
            const auto output = processor.process(sample());
            assert(output.detected == GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        processor.process(sample(1.0F, 120.0F, 100.0F));
        processor.process(sample(1.0F, 125.0F, 105.0F));
        processor.process(sample(1.0F, 10.0F));
        processor.process(sample(1.0F, -110.0F, -100.0F));
        processor.process(sample(1.0F, -115.0F, -105.0F));
        for (int index = 0; index < 4; ++index) {
            assert(processor.process(sample()).detected == GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        for (int index = 0; index < 12; ++index) {
            GestureSample z_only = sample();
            z_only.gyro_z_dps = index < 4 ? 220.0F : -220.0F;
            assert(processor.process(z_only).detected == GestureKind::none);
        }
    }
    {
        now_us = 0;
        GestureProcessor processor;
        const auto perform_twist = [&processor]() {
            processor.process(sample(1.0F, 120.0F));
            processor.process(sample(1.0F, 125.0F));
            processor.process(sample(1.0F, 15.0F));
            processor.process(sample(1.0F, 12.0F));
            processor.process(sample(1.0F, 10.0F));
            processor.process(sample(1.0F, -105.0F));
            processor.process(sample(1.0F, -110.0F));
            processor.process(sample(1.0F, 10.0F));
            processor.process(sample(1.0F, 8.0F));
            return processor.process(sample(1.0F, 6.0F));
        };
        assert(perform_twist().detected == GestureKind::double_twist);
        assert(perform_twist().detected == GestureKind::none);
        settle(processor, 40);
        assert(perform_twist().detected == GestureKind::double_twist);
    }
    {
        now_us = 0;
        auto profile = nightglass::services::default_gesture_profile();
        profile.twist_peak_dps = 150.0F;
        GestureProcessor processor(profile);
        const auto perform_twist = [&processor](float peak) {
            processor.process(sample(1.0F, peak));
            processor.process(sample(1.0F, peak + 5.0F));
            processor.process(sample(1.0F, 15.0F));
            processor.process(sample(1.0F, 12.0F));
            processor.process(sample(1.0F, 10.0F));
            processor.process(sample(1.0F, -peak));
            processor.process(sample(1.0F, -peak - 5.0F));
            processor.process(sample(1.0F, 10.0F));
            processor.process(sample(1.0F, 8.0F));
            return processor.process(sample(1.0F, 6.0F));
        };
        assert(perform_twist(125.0F).detected == GestureKind::none);
        assert(perform_twist(165.0F).detected == GestureKind::double_twist);
    }
    {
        auto invalid = nightglass::services::default_gesture_profile();
        invalid.twist_peak_dps = std::numeric_limits<float>::quiet_NaN();
        GestureProcessor processor(invalid);
        assert(processor.profile().twist_peak_dps ==
               nightglass::services::default_gesture_profile().twist_peak_dps);
        assert(nightglass::services::valid_gesture_profile(
            nightglass::services::training_gesture_profile()));
    }
    return 0;
}
