#include <cassert>
#include <cstdint>

#include "nightglass/services/gesture_calibration.hpp"

using nightglass::services::GestureCalibrationResult;
using nightglass::services::GestureCalibrationStage;
using nightglass::services::GestureCalibrator;
using nightglass::services::GestureSample;

namespace {

std::int64_t now_us = 1'000'000;

GestureSample sample(float az = 1.0F, float gx = 0.0F, float gy = 0.0F,
                     float ax = 0.0F) {
    now_us += 40'000;
    return {.accel_x_g = ax,
            .accel_y_g = 0.0F,
            .accel_z_g = az,
            .gyro_x_dps = gx,
            .gyro_y_dps = gy,
            .gyro_z_dps = 0.0F,
            .sampled_at_us = now_us,
            .allow_raise = true,
            .gyro_calibrated = true};
}

void begin_capture(GestureCalibrator &calibrator) {
    assert(calibrator.request_capture(now_us));
    now_us += GestureCalibrator::kCountdownUs;
}

void perform_raise(GestureCalibrator &calibrator) {
    begin_capture(calibrator);
    // A labelled capture must tolerate a natural, partly face-up starting pose
    // and a slower transition than the strict always-on recognizer accepts.
    for (int index = 0; index < 20; ++index) {
        calibrator.process(sample(-0.45F, 0.0F, 0.0F, 0.89F));
    }
    calibrator.process(sample(-0.55F, 30.0F, 0.0F, 0.83F));
    calibrator.process(sample(-0.65F, 28.0F, 0.0F, 0.76F));
    calibrator.process(sample(-0.75F, 8.0F, 0.0F, 0.66F));
    calibrator.process(sample(-0.77F, 4.0F, 0.0F, 0.64F));
    calibrator.process(sample(-0.79F, 3.0F, 0.0F, 0.61F));
}

void perform_twist(GestureCalibrator &calibrator) {
    begin_capture(calibrator);
    calibrator.process(sample(1.0F, 125.0F));
    calibrator.process(sample(1.0F, 130.0F));
    calibrator.process(sample(1.0F, 15.0F));
    calibrator.process(sample(1.0F, 12.0F));
    calibrator.process(sample(1.0F, 10.0F));
    calibrator.process(sample(1.0F, -112.0F));
    calibrator.process(sample(1.0F, -118.0F));
    calibrator.process(sample(1.0F, 10.0F));
    calibrator.process(sample(1.0F, 8.0F));
    calibrator.process(sample(1.0F, 6.0F));
}

void perform_shake(GestureCalibrator &calibrator) {
    begin_capture(calibrator);
    calibrator.process(sample(1.0F, 85.0F, 0.0F, 1.42F));
    calibrator.process(sample());
    calibrator.process(sample(1.0F, -85.0F, 0.0F, -1.42F));
    calibrator.process(sample());
    calibrator.process(sample(1.0F, 85.0F, 0.0F, 1.45F));
    calibrator.process(sample());
    calibrator.process(sample(1.0F, -85.0F, 0.0F, -1.45F));
}

void perform_flick(GestureCalibrator &calibrator) {
    begin_capture(calibrator);
    calibrator.process(sample(1.0F, 205.0F, 0.0F, 0.70F));
    calibrator.process(sample(1.0F, 210.0F, 0.0F, 0.70F));
    calibrator.process(sample(1.0F, 10.0F));
    calibrator.process(sample(1.0F, -80.0F));
    calibrator.process(sample(1.0F, 10.0F));
    calibrator.process(sample(1.0F, 8.0F));
    calibrator.process(sample(1.0F, 6.0F));
}

void complete_examples(GestureCalibrator &calibrator) {
    calibrator.start(now_us);
    calibrator.process(sample());
    assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::raise);
    for (int index = 0; index < 3; ++index) perform_raise(calibrator);
    assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::double_twist);
    for (int index = 0; index < 3; ++index) perform_twist(calibrator);
    assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::shake);
    for (int index = 0; index < 3; ++index) perform_shake(calibrator);
    assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::flick);
    for (int index = 0; index < 3; ++index) perform_flick(calibrator);
    assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::quiet);
}

void finish_clean_quiet_window(GestureCalibrator &calibrator) {
    begin_capture(calibrator);
    const auto finish_us = now_us + GestureCalibrator::kQuietCaptureUs + 80'000;
    while (now_us <= finish_us) calibrator.process(sample());
}

}  // namespace

int main() {
    {
        GestureCalibrator calibrator;
        complete_examples(calibrator);
        finish_clean_quiet_window(calibrator);
        const auto completed = calibrator.snapshot(now_us);
        assert(completed.stage == GestureCalibrationStage::complete);
        assert(completed.last_result == GestureCalibrationResult::profile_saved);
        nightglass::services::GestureProfile profile{};
        assert(calibrator.take_completed_profile(profile));
        assert(profile.calibrated == 1);
        assert(nightglass::services::valid_gesture_profile(profile));
        assert(profile.raise_face_up_g > 0.55F);
        assert(profile.twist_peak_dps >= 65.0F);
        assert(profile.shake_peak_g >= 0.22F);
        assert(profile.flick_peak_dps >= 110.0F);
        assert(!calibrator.take_completed_profile(profile));
    }
    {
        now_us += 1'000'000;
        GestureCalibrator calibrator;
        complete_examples(calibrator);
        begin_capture(calibrator);
        // An intentional-looking twist during the quiet check must force a
        // retry and tighten the candidate rather than saving immediately.
        calibrator.process(sample(1.0F, 150.0F));
        calibrator.process(sample(1.0F, 155.0F));
        calibrator.process(sample(1.0F, 10.0F));
        calibrator.process(sample(1.0F, 8.0F));
        calibrator.process(sample(1.0F, 6.0F));
        calibrator.process(sample(1.0F, -135.0F));
        calibrator.process(sample(1.0F, -140.0F));
        calibrator.process(sample(1.0F, 10.0F));
        calibrator.process(sample(1.0F, 8.0F));
        calibrator.process(sample(1.0F, 6.0F));
        const auto finish_us = now_us + GestureCalibrator::kQuietCaptureUs + 80'000;
        while (now_us <= finish_us) calibrator.process(sample());
        const auto retry = calibrator.snapshot(now_us);
        assert(retry.stage == GestureCalibrationStage::quiet);
        assert(retry.last_result == GestureCalibrationResult::quiet_retry);
        assert(retry.quiet_false_positives > 0);
        finish_clean_quiet_window(calibrator);
        assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::complete);
    }
    {
        now_us += 1'000'000;
        GestureCalibrator calibrator;
        calibrator.start(now_us);
        calibrator.process(sample());
        begin_capture(calibrator);
        calibrator.process(sample());
        now_us += GestureCalibrator::kRaiseCaptureUs + 40'000;
        calibrator.process(sample());
        const auto missed = calibrator.snapshot(now_us);
        assert(missed.stage == GestureCalibrationStage::raise);
        assert(missed.last_result == GestureCalibrationResult::sample_missed);
        assert(missed.attempts == 1);
        calibrator.cancel();
        assert(!calibrator.active());
        assert(calibrator.snapshot(now_us).stage == GestureCalibrationStage::idle);
    }
    return 0;
}
