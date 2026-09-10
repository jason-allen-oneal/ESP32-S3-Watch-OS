#include "nightglass/services/gesture_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nightglass::services {
namespace {

constexpr float kRaiseMotionDps = 20.0F;
constexpr float kRaiseMaximumDps = 300.0F;
constexpr float kRaiseFaceUpZ = -0.50F;
constexpr float kCaptureSettleDps = 35.0F;
constexpr float kTwistPeakDps = 45.0F;
constexpr float kFlickPeakDps = 80.0F;
constexpr float kShakePeakG = 0.16F;
constexpr float kShakeRearmG = 0.10F;
constexpr float kShakeGyroDps = 35.0F;
constexpr float kAxisDominance = 1.15F;
constexpr std::int64_t kShakeMaximumGapUs = 1'000'000;
constexpr std::int64_t kShakeMinimumSpanUs = 240'000;

float magnitude(float x, float y, float z) noexcept {
    return std::sqrt(x * x + y * y + z * z);
}

bool finite_sample(const GestureSample &sample) noexcept {
    return std::isfinite(sample.accel_x_g) && std::isfinite(sample.accel_y_g) &&
           std::isfinite(sample.accel_z_g) && std::isfinite(sample.gyro_x_dps) &&
           std::isfinite(sample.gyro_y_dps) && std::isfinite(sample.gyro_z_dps) &&
           sample.sampled_at_us > 0;
}

bool gravity_band(float value, float minimum, float maximum) noexcept {
    return value >= minimum && value <= maximum;
}

std::uint32_t remaining_ms(std::int64_t deadline_us, std::int64_t now_us) noexcept {
    if (deadline_us <= now_us) return 0;
    return static_cast<std::uint32_t>((deadline_us - now_us + 999) / 1000);
}

}  // namespace

void GestureCalibrator::start(std::int64_t) noexcept {
    stage_ = GestureCalibrationStage::stationary;
    phase_ = GestureCapturePhase::waiting;
    last_result_ = GestureCalibrationResult::none;
    candidate_profile_ = default_gesture_profile();
    quiet_processor_.set_profile(candidate_profile_);
    deadline_us_ = 0;
    repetitions_ = 0;
    attempts_ = 0;
    quiet_false_positives_ = 0;
    minimum_strength_ = 0.0F;
    minimum_gyro_strength_ = 0.0F;
    capture_gyro_strength_ = 0.0F;
    reset_capture();
    quiet_window_dirty_ = false;
    completion_pending_ = false;
}

void GestureCalibrator::cancel() noexcept {
    stage_ = GestureCalibrationStage::idle;
    phase_ = GestureCapturePhase::waiting;
    last_result_ = GestureCalibrationResult::none;
    deadline_us_ = 0;
    completion_pending_ = false;
    reset_capture();
    quiet_processor_.reset();
}

bool GestureCalibrator::request_capture(std::int64_t now_us) noexcept {
    const bool gesture_stage = stage_ == GestureCalibrationStage::raise ||
                               stage_ == GestureCalibrationStage::double_twist ||
                               stage_ == GestureCalibrationStage::shake ||
                               stage_ == GestureCalibrationStage::flick;
    if ((!gesture_stage && stage_ != GestureCalibrationStage::quiet) ||
        phase_ != GestureCapturePhase::waiting || now_us <= 0) {
        return false;
    }
    phase_ = GestureCapturePhase::countdown;
    last_result_ = GestureCalibrationResult::none;
    deadline_us_ = now_us + kCountdownUs;
    reset_capture();
    return true;
}

void GestureCalibrator::begin_recording(std::int64_t now_us) noexcept {
    phase_ = GestureCapturePhase::recording;
    reset_capture();
    if (stage_ == GestureCalibrationStage::quiet) {
        quiet_processor_.set_profile(candidate_profile_);
        quiet_window_dirty_ = false;
        deadline_us_ = now_us + kQuietCaptureUs;
    } else {
        deadline_us_ = now_us +
                       (stage_ == GestureCalibrationStage::raise ? kRaiseCaptureUs
                                                                 : kGestureCaptureUs);
    }
}

void GestureCalibrator::reset_capture() noexcept {
    capture_gyro_strength_ = 0.0F;
    capture_peak_strength_ = 0.0F;
    capture_opposite_strength_ = 0.0F;
    capture_acceleration_strength_ = 0.0F;
    capture_first_peak_us_ = 0;
    capture_last_peak_us_ = 0;
    capture_axis_ = 0;
    capture_sign_ = 0;
    capture_peak_samples_ = 0;
    capture_settle_samples_ = 0;
    capture_opposite_samples_ = 0;
    capture_shake_peaks_ = 0;
    capture_motion_seen_ = false;
    capture_motion_settled_ = false;
    capture_shake_armed_ = true;
}

void GestureCalibrator::process(const GestureSample &sample) noexcept {
    if (stage_ == GestureCalibrationStage::stationary) {
        if (sample.gyro_calibrated) {
            stage_ = GestureCalibrationStage::raise;
            repetitions_ = 0;
            attempts_ = 0;
            last_result_ = GestureCalibrationResult::none;
        }
        return;
    }
    if (!active() || !sample.gyro_calibrated) return;

    if (phase_ == GestureCapturePhase::countdown) {
        if (sample.sampled_at_us < deadline_us_) return;
        begin_recording(sample.sampled_at_us);
    }
    if (phase_ != GestureCapturePhase::recording) return;

    if (sample.sampled_at_us > deadline_us_) {
        phase_ = GestureCapturePhase::waiting;
        ++attempts_;
        if (stage_ == GestureCalibrationStage::quiet) {
            if (quiet_window_dirty_) {
                last_result_ = GestureCalibrationResult::quiet_retry;
                quiet_window_dirty_ = false;
            } else {
                candidate_profile_.calibrated = 1;
                stage_ = GestureCalibrationStage::complete;
                last_result_ = GestureCalibrationResult::profile_saved;
                completion_pending_ = true;
            }
        } else {
            last_result_ = GestureCalibrationResult::sample_missed;
        }
        return;
    }

    if (stage_ == GestureCalibrationStage::quiet) {
        auto recognizer_sample = sample;
        recognizer_sample.allow_raise = true;
        recognizer_sample.gyro_calibrated = true;
        const auto output = quiet_processor_.process(recognizer_sample);
        if (output.detected != GestureKind::none) {
            ++quiet_false_positives_;
            quiet_window_dirty_ = true;
            tune_from_false_positive(output.detected, output.strength);
        }
        return;
    }

    process_labelled_capture(sample);
}

void GestureCalibrator::process_labelled_capture(const GestureSample &sample) noexcept {
    if (!finite_sample(sample)) return;
    const float accel_magnitude = magnitude(sample.accel_x_g, sample.accel_y_g,
                                            sample.accel_z_g);
    const float gyro_magnitude = magnitude(sample.gyro_x_dps, sample.gyro_y_dps,
                                           sample.gyro_z_dps);
    if (!std::isfinite(accel_magnitude) || !std::isfinite(gyro_magnitude) ||
        accel_magnitude < 0.2F || accel_magnitude > 2.5F) {
        return;
    }
    const float acceleration_delta = std::fabs(accel_magnitude - 1.0F);
    const float pitch_roll = std::max(std::fabs(sample.gyro_x_dps),
                                      std::fabs(sample.gyro_y_dps));
    capture_gyro_strength_ = std::max(capture_gyro_strength_, gyro_magnitude);
    capture_acceleration_strength_ =
        std::max(capture_acceleration_strength_, acceleration_delta);

    if (stage_ == GestureCalibrationStage::raise) {
        capture_peak_strength_ = std::max(capture_peak_strength_, -sample.accel_z_g);
        if (pitch_roll >= kRaiseMotionDps && pitch_roll <= kRaiseMaximumDps) {
            capture_motion_seen_ = true;
        }
        if (capture_motion_seen_ && sample.accel_z_g <= kRaiseFaceUpZ &&
            gravity_band(accel_magnitude, 0.65F, 1.35F) &&
            gyro_magnitude < kCaptureSettleDps) {
            if (++capture_settle_samples_ >= 3) {
                accept_sample(capture_peak_strength_, capture_gyro_strength_);
            }
        } else if (sample.accel_z_g > kRaiseFaceUpZ + 0.12F ||
                   gyro_magnitude >= kCaptureSettleDps) {
            capture_settle_samples_ = 0;
        }
        return;
    }

    const float rotation_axes[2]{sample.gyro_x_dps, sample.gyro_y_dps};
    const std::uint8_t dominant_axis =
        std::fabs(rotation_axes[1]) > std::fabs(rotation_axes[0]) ? 1U : 0U;
    const float dominant_value = rotation_axes[dominant_axis];
    const float dominant_strength = std::fabs(dominant_value);
    const float other_strength = std::fabs(rotation_axes[1U - dominant_axis]);

    if (stage_ == GestureCalibrationStage::double_twist) {
        if (!capture_motion_seen_) {
            if (dominant_strength >= kTwistPeakDps &&
                dominant_strength >= other_strength * kAxisDominance) {
                const auto sign = static_cast<std::int8_t>(dominant_value >= 0.0F ? 1 : -1);
                if (capture_peak_samples_ == 0 || capture_axis_ != dominant_axis ||
                    capture_sign_ != sign) {
                    capture_axis_ = dominant_axis;
                    capture_sign_ = sign;
                    capture_peak_samples_ = 1;
                    capture_peak_strength_ = dominant_strength;
                } else {
                    ++capture_peak_samples_;
                    capture_peak_strength_ =
                        std::max(capture_peak_strength_, dominant_strength);
                }
                if (capture_peak_samples_ >= 2) {
                    capture_motion_seen_ = true;
                    capture_first_peak_us_ = sample.sampled_at_us;
                }
            } else if (dominant_strength < kCaptureSettleDps) {
                capture_peak_samples_ = 0;
            }
            return;
        }

        const float tracked_value = rotation_axes[capture_axis_];
        const float tracked_strength = std::fabs(tracked_value);
        if (!capture_motion_settled_) {
            if (tracked_strength < kCaptureSettleDps) {
                if (++capture_settle_samples_ >= 2) capture_motion_settled_ = true;
            } else {
                capture_settle_samples_ = 0;
                if ((tracked_value >= 0.0F ? 1 : -1) == capture_sign_) {
                    capture_peak_strength_ =
                        std::max(capture_peak_strength_, tracked_strength);
                }
            }
            return;
        }

        const bool opposite = (tracked_value >= 0.0F ? 1 : -1) != capture_sign_;
        if (opposite && tracked_strength >= kTwistPeakDps) {
            ++capture_opposite_samples_;
            capture_opposite_strength_ =
                std::max(capture_opposite_strength_, tracked_strength);
            if (capture_opposite_samples_ >= 2) {
                accept_sample(std::max(capture_peak_strength_, capture_opposite_strength_),
                              capture_gyro_strength_);
            }
        } else if (tracked_strength < kCaptureSettleDps) {
            capture_opposite_samples_ = 0;
        }
        return;
    }

    if (stage_ == GestureCalibrationStage::shake) {
        if (capture_last_peak_us_ != 0 &&
            sample.sampled_at_us - capture_last_peak_us_ > kShakeMaximumGapUs) {
            capture_shake_peaks_ = 0;
            capture_first_peak_us_ = 0;
            capture_shake_armed_ = true;
        }
        if (acceleration_delta <= kShakeRearmG) capture_shake_armed_ = true;
        if (capture_shake_armed_ && acceleration_delta >= kShakePeakG &&
            gyro_magnitude >= kShakeGyroDps) {
            if (capture_first_peak_us_ == 0) capture_first_peak_us_ = sample.sampled_at_us;
            capture_last_peak_us_ = sample.sampled_at_us;
            ++capture_shake_peaks_;
            capture_shake_armed_ = false;
            if (capture_shake_peaks_ >= 4 &&
                capture_last_peak_us_ - capture_first_peak_us_ >= kShakeMinimumSpanUs) {
                accept_sample(capture_acceleration_strength_, capture_gyro_strength_);
            }
        }
        return;
    }

    if (stage_ == GestureCalibrationStage::flick) {
        if (!capture_motion_seen_ && dominant_strength >= kFlickPeakDps &&
            dominant_strength >= other_strength * kAxisDominance &&
            acceleration_delta >= 0.05F) {
            capture_motion_seen_ = true;
            capture_axis_ = dominant_axis;
            capture_peak_strength_ = dominant_strength;
        } else if (capture_motion_seen_) {
            capture_peak_strength_ = std::max(
                capture_peak_strength_, std::fabs(rotation_axes[capture_axis_]));
            if (gyro_magnitude < kCaptureSettleDps) {
                if (++capture_settle_samples_ >= 3) {
                    accept_sample(capture_peak_strength_, capture_gyro_strength_);
                }
            } else {
                capture_settle_samples_ = 0;
            }
        }
    }
}

void GestureCalibrator::accept_sample(float strength, float gyro_strength) noexcept {
    if (!std::isfinite(strength) || strength <= 0.0F ||
        !std::isfinite(gyro_strength) || gyro_strength <= 0.0F) {
        return;
    }
    if (repetitions_ == 0) {
        minimum_strength_ = strength;
        minimum_gyro_strength_ = gyro_strength;
    } else {
        minimum_strength_ = std::min(minimum_strength_, strength);
        minimum_gyro_strength_ = std::min(minimum_gyro_strength_, gyro_strength);
    }
    ++repetitions_;
    last_result_ = GestureCalibrationResult::sample_ok;
    phase_ = GestureCapturePhase::waiting;
    deadline_us_ = 0;
    if (repetitions_ >= kRequiredRepetitions) advance_stage();
}

void GestureCalibrator::advance_stage() noexcept {
    switch (stage_) {
        case GestureCalibrationStage::raise:
            candidate_profile_.raise_face_up_g =
                std::clamp(minimum_strength_ * 0.85F, 0.55F, 0.85F);
            stage_ = GestureCalibrationStage::double_twist;
            break;
        case GestureCalibrationStage::double_twist:
            candidate_profile_.twist_peak_dps =
                std::clamp(minimum_strength_ * 0.70F, 65.0F, 145.0F);
            stage_ = GestureCalibrationStage::shake;
            break;
        case GestureCalibrationStage::shake:
            candidate_profile_.shake_peak_g =
                std::clamp(minimum_strength_ * 0.70F, 0.22F, 0.48F);
            candidate_profile_.shake_gyro_dps =
                std::clamp(minimum_gyro_strength_ * 0.65F, 45.0F, 100.0F);
            stage_ = GestureCalibrationStage::flick;
            break;
        case GestureCalibrationStage::flick:
            candidate_profile_.flick_peak_dps =
                std::clamp(minimum_strength_ * 0.70F, 110.0F, 250.0F);
            stage_ = GestureCalibrationStage::quiet;
            break;
        case GestureCalibrationStage::idle:
        case GestureCalibrationStage::stationary:
        case GestureCalibrationStage::quiet:
        case GestureCalibrationStage::complete: return;
    }
    repetitions_ = 0;
    attempts_ = 0;
    minimum_strength_ = 0.0F;
    minimum_gyro_strength_ = 0.0F;
    capture_gyro_strength_ = 0.0F;
    reset_capture();
    phase_ = GestureCapturePhase::waiting;
    last_result_ = GestureCalibrationResult::none;
}

void GestureCalibrator::tune_from_false_positive(GestureKind kind,
                                                  float strength) noexcept {
    if (!std::isfinite(strength) || strength <= 0.0F) return;
    switch (kind) {
        case GestureKind::raise:
            candidate_profile_.raise_face_up_g = std::clamp(
                std::max(candidate_profile_.raise_face_up_g, strength + 0.05F),
                0.55F, 0.92F);
            break;
        case GestureKind::double_twist:
            candidate_profile_.twist_peak_dps = std::clamp(
                std::max(candidate_profile_.twist_peak_dps, strength * 1.10F),
                65.0F, 180.0F);
            break;
        case GestureKind::shake:
            candidate_profile_.shake_peak_g = std::clamp(
                std::max(candidate_profile_.shake_peak_g, strength * 1.10F),
                0.22F, 0.60F);
            break;
        case GestureKind::flick:
            candidate_profile_.flick_peak_dps = std::clamp(
                std::max(candidate_profile_.flick_peak_dps, strength * 1.10F),
                110.0F, 290.0F);
            break;
        case GestureKind::none: break;
    }
}

GestureCalibrationSnapshot GestureCalibrator::snapshot(std::int64_t now_us) const noexcept {
    return {
        .stage = stage_,
        .phase = phase_,
        .last_result = last_result_,
        .repetitions = repetitions_,
        .repetitions_required = kRequiredRepetitions,
        .attempts = attempts_,
        .quiet_false_positives = quiet_false_positives_,
        .remaining_ms = phase_ == GestureCapturePhase::waiting
                            ? 0U
                            : remaining_ms(deadline_us_, now_us),
        .completion_pending = completion_pending_,
    };
}

bool GestureCalibrator::active() const noexcept {
    return stage_ != GestureCalibrationStage::idle &&
           stage_ != GestureCalibrationStage::complete;
}

bool GestureCalibrator::take_completed_profile(GestureProfile &profile) noexcept {
    if (!completion_pending_ || stage_ != GestureCalibrationStage::complete ||
        !valid_gesture_profile(candidate_profile_)) {
        return false;
    }
    profile = candidate_profile_;
    completion_pending_ = false;
    return true;
}

bool gesture_calibration_active(const GestureCalibrationSnapshot &snapshot) noexcept {
    return snapshot.stage != GestureCalibrationStage::idle &&
           snapshot.stage != GestureCalibrationStage::complete;
}

const char *gesture_calibration_stage_name(GestureCalibrationStage stage) noexcept {
    switch (stage) {
        case GestureCalibrationStage::stationary: return "SENSOR ZERO";
        case GestureCalibrationStage::raise: return "RAISE";
        case GestureCalibrationStage::double_twist: return "DOUBLE TWIST";
        case GestureCalibrationStage::shake: return "SHAKE";
        case GestureCalibrationStage::flick: return "FLICK";
        case GestureCalibrationStage::quiet: return "QUIET CHECK";
        case GestureCalibrationStage::complete: return "COMPLETE";
        case GestureCalibrationStage::idle: break;
    }
    return "IDLE";
}

}  // namespace nightglass::services
