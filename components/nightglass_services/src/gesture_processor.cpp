#include "nightglass/services/gesture_processor.hpp"

#include <algorithm>
#include <cmath>

namespace nightglass::services {
namespace {

constexpr std::int64_t kMaximumSampleGapUs = 160'000;
constexpr std::int64_t kRaiseCooldownUs = 2'500'000;
constexpr std::int64_t kTwistCooldownUs = 1'500'000;
constexpr std::int64_t kShakeCooldownUs = 3'000'000;
constexpr std::int64_t kFlickCooldownUs = 1'200'000;
// QMI8658 Z is negative when the fitted display faces upward.
constexpr float kRaiseMotionDps = 35.0F;
constexpr float kRaiseMaximumDps = 220.0F;
constexpr float kRaiseSettleDps = 35.0F;
constexpr std::int64_t kRaiseWindowUs = 2'000'000;
constexpr float kShakeRearmG = 0.15F;
constexpr std::int64_t kShakeMinimumPeakGapUs = 80'000;
constexpr std::int64_t kShakeMaximumPeakGapUs = 240'000;
constexpr std::int64_t kShakeWindowUs = 1'000'000;
constexpr float kRotationSettleDps = 35.0F;
constexpr float kFlickBrakeDps = 60.0F;
constexpr float kAxisDominance = 1.4F;
constexpr std::int64_t kOppositeStartUs = 450'000;
constexpr std::int64_t kTwistWindowUs = 900'000;
constexpr std::int64_t kFlickBrakeWindowUs = 200'000;

bool finite_sample(const GestureSample &sample) noexcept {
    return std::isfinite(sample.accel_x_g) && std::isfinite(sample.accel_y_g) &&
           std::isfinite(sample.accel_z_g) && std::isfinite(sample.gyro_x_dps) &&
           std::isfinite(sample.gyro_y_dps) && std::isfinite(sample.gyro_z_dps) &&
           sample.sampled_at_us > 0;
}

float magnitude(float x, float y, float z) noexcept {
    return std::sqrt(x * x + y * y + z * z);
}

bool gravity_band(float magnitude_g, float minimum, float maximum) noexcept {
    return magnitude_g >= minimum && magnitude_g <= maximum;
}

bool finite_profile(const GestureProfile &profile) noexcept {
    return std::isfinite(profile.raise_face_up_g) &&
           std::isfinite(profile.twist_peak_dps) &&
           std::isfinite(profile.shake_peak_g) &&
           std::isfinite(profile.shake_gyro_dps) &&
           std::isfinite(profile.flick_peak_dps);
}

}  // namespace

GestureProfile default_gesture_profile() noexcept { return {}; }

GestureProfile training_gesture_profile() noexcept {
    GestureProfile profile{};
    profile.raise_face_up_g = 0.55F;
    profile.twist_peak_dps = 60.0F;
    profile.shake_peak_g = 0.20F;
    profile.shake_gyro_dps = 45.0F;
    profile.flick_peak_dps = 100.0F;
    return profile;
}

bool valid_gesture_profile(const GestureProfile &profile) noexcept {
    return profile.format_version == GestureProfile::kFormatVersion &&
           finite_profile(profile) && profile.raise_face_up_g >= 0.50F &&
           profile.raise_face_up_g <= 0.95F &&
           profile.twist_peak_dps >= 55.0F && profile.twist_peak_dps <= 200.0F &&
           profile.shake_peak_g >= 0.18F && profile.shake_peak_g <= 0.70F &&
           profile.shake_gyro_dps >= 40.0F && profile.shake_gyro_dps <= 150.0F &&
           profile.flick_peak_dps >= 90.0F && profile.flick_peak_dps <= 320.0F &&
           (profile.calibrated == 0 || profile.calibrated == 1);
}

GestureProcessor::GestureProcessor(const GestureProfile &profile) noexcept {
    set_profile(profile);
}

void GestureProcessor::set_profile(const GestureProfile &profile) noexcept {
    profile_ = valid_gesture_profile(profile) ? profile : default_gesture_profile();
    reset_transient_state();
    last_sample_us_ = 0;
    cooldown_until_us_ = 0;
}

GestureProcessorOutput GestureProcessor::process(const GestureSample &sample) noexcept {
    GestureProcessorOutput output{};
    if (!finite_sample(sample) || sample.sampled_at_us <= last_sample_us_) {
        reset_transient_state();
        return output;
    }
    const float accel_magnitude = magnitude(sample.accel_x_g, sample.accel_y_g,
                                            sample.accel_z_g);
    const float gyro_magnitude = magnitude(sample.gyro_x_dps, sample.gyro_y_dps,
                                           sample.gyro_z_dps);
    if (!std::isfinite(accel_magnitude) || accel_magnitude < 0.2F ||
        accel_magnitude > 2.5F || !std::isfinite(gyro_magnitude)) {
        reset_transient_state();
        return output;
    }
    if (last_sample_us_ > 0 && sample.sampled_at_us - last_sample_us_ > kMaximumSampleGapUs) {
        reset_transient_state();
    }
    last_sample_us_ = sample.sampled_at_us;
    output.sample_valid = true;
    const float acceleration_delta = std::fabs(accel_magnitude - 1.0F);
    if (sample.sampled_at_us < cooldown_until_us_) {
        reset_transient_state();
        return output;
    }

    if (!sample.allow_raise) {
        raise_armed_ = false;
        raise_arm_samples_ = 0;
        raise_rotation_samples_ = 0;
        raise_confirm_samples_ = 0;
        raise_rotation_seen_ = false;
    } else if (!raise_armed_) {
        // Arm from any meaningfully less face-up pose than the learned finish.
        // A fixed near-vertical gate rejected ordinary arm-down starting poses
        // on the fitted watch even though their final face-up pose was clear.
        const float raise_arm_z = -profile_.raise_face_up_g + 0.18F;
        if (sample.accel_z_g >= raise_arm_z &&
            gravity_band(accel_magnitude, 0.75F, 1.25F)) {
            if (++raise_arm_samples_ >= 8) {
                raise_armed_ = true;
                raise_armed_at_us_ = sample.sampled_at_us;
            }
        } else {
            raise_arm_samples_ = 0;
        }
    } else if (sample.sampled_at_us - raise_armed_at_us_ > kRaiseWindowUs) {
        raise_armed_ = false;
        raise_arm_samples_ = 0;
        raise_rotation_samples_ = 0;
        raise_confirm_samples_ = 0;
        raise_rotation_seen_ = false;
    } else {
        const float pitch_roll = std::max(std::fabs(sample.gyro_x_dps),
                                          std::fabs(sample.gyro_y_dps));
        if (!raise_rotation_seen_ && pitch_roll >= kRaiseMotionDps &&
            pitch_roll <= kRaiseMaximumDps) {
            if (++raise_rotation_samples_ >= 2) raise_rotation_seen_ = true;
        } else if (!raise_rotation_seen_ && pitch_roll < kRaiseMotionDps) {
            raise_rotation_samples_ = 0;
        }
        const float raise_face_up_z = -profile_.raise_face_up_g;
        if (raise_rotation_seen_ && sample.accel_z_g <= raise_face_up_z &&
            gravity_band(accel_magnitude, 0.75F, 1.25F) &&
            gyro_magnitude < kRaiseSettleDps) {
            ++raise_confirm_samples_;
        } else if (sample.accel_z_g > raise_face_up_z + 0.12F ||
                   gyro_magnitude >= kRaiseSettleDps) {
            raise_confirm_samples_ = 0;
        }
    }

    if (!sample.gyro_calibrated) {
        shake_armed_ = true;
        shake_peaks_ = 0;
        shake_window_started_us_ = 0;
        last_shake_peak_us_ = 0;
        rotation_active_ = false;
        rotation_settled_ = false;
        rotation_peak_samples_ = 0;
        opposite_peak_samples_ = 0;
        settle_samples_ = 0;
    }
    if (sample.gyro_calibrated && acceleration_delta <= kShakeRearmG) {
        shake_armed_ = true;
    }
    if (sample.gyro_calibrated && acceleration_delta >= profile_.shake_peak_g &&
        gyro_magnitude >= profile_.shake_gyro_dps &&
        shake_armed_) {
        const float acceleration_axes[3]{sample.accel_x_g, sample.accel_y_g,
                                         sample.accel_z_g};
        std::uint8_t shake_axis = 0;
        if (std::fabs(acceleration_axes[1]) > std::fabs(acceleration_axes[shake_axis])) {
            shake_axis = 1;
        }
        if (std::fabs(acceleration_axes[2]) > std::fabs(acceleration_axes[shake_axis])) {
            shake_axis = 2;
        }
        const auto shake_sign = static_cast<std::int8_t>(
            acceleration_axes[shake_axis] >= 0.0F ? 1 : -1);
        const auto gap = last_shake_peak_us_ == 0 ? 0
                                                  : sample.sampled_at_us - last_shake_peak_us_;
        if (last_shake_peak_us_ != 0 &&
            (gap < kShakeMinimumPeakGapUs || gap > kShakeMaximumPeakGapUs ||
             shake_axis != shake_axis_ || shake_sign == shake_sign_)) {
            shake_peaks_ = 0;
            shake_window_started_us_ = 0;
        }
        if (shake_window_started_us_ == 0) shake_window_started_us_ = sample.sampled_at_us;
        ++shake_peaks_;
        shake_axis_ = shake_axis;
        shake_sign_ = shake_sign;
        last_shake_peak_us_ = sample.sampled_at_us;
        shake_armed_ = false;
    }

    const float axes[2]{sample.gyro_x_dps, sample.gyro_y_dps};
    const std::uint8_t dominant_axis =
        std::fabs(axes[1]) > std::fabs(axes[0]) ? 1U : 0U;
    const float dominant_value = axes[dominant_axis];
    const float dominant_strength = std::fabs(dominant_value);
    const float other_strength = std::fabs(axes[1U - dominant_axis]);
    // Rotation capture is shared by twist and flick. Arm at the less sensitive
    // of their two learned gates, then apply the gesture-specific gate when
    // classifying the completed shape.
    const float rotation_arm_dps =
        std::min(profile_.twist_peak_dps, profile_.flick_peak_dps);
    const bool dominant = dominant_strength >= rotation_arm_dps &&
                          dominant_strength >= other_strength * kAxisDominance;

    if (sample.gyro_calibrated && !rotation_active_) {
        if (dominant && gravity_band(accel_magnitude, 0.70F, 1.30F)) {
            const auto sign = static_cast<std::int8_t>(dominant_value >= 0.0F ? 1 : -1);
            if (rotation_peak_samples_ == 0 || rotation_axis_ != dominant_axis ||
                rotation_sign_ != sign) {
                rotation_peak_samples_ = 1;
                rotation_axis_ = dominant_axis;
                rotation_sign_ = sign;
                rotation_strength_ = dominant_strength;
                rotation_accel_delta_ = acceleration_delta;
                rotation_started_us_ = sample.sampled_at_us;
            } else {
                ++rotation_peak_samples_;
                rotation_strength_ = std::max(rotation_strength_, dominant_strength);
                rotation_accel_delta_ = std::max(rotation_accel_delta_, acceleration_delta);
            }
            if (rotation_peak_samples_ >= 2) rotation_active_ = true;
        } else {
            rotation_peak_samples_ = 0;
        }
    } else if (sample.gyro_calibrated) {
        const float tracked_value = axes[rotation_axis_];
        const float tracked_strength = std::fabs(tracked_value);
        const bool opposite = (tracked_value >= 0.0F ? 1 : -1) != rotation_sign_;
        if (!rotation_settled_) {
            if (tracked_strength < kRotationSettleDps) {
                rotation_settled_ = true;
                rotation_first_ended_us_ = sample.sampled_at_us;
            }
        } else if (opposite && tracked_strength >= kFlickBrakeDps &&
                   sample.sampled_at_us - rotation_first_ended_us_ <= kOppositeStartUs) {
            if (opposite_peak_samples_ == 0) opposite_started_us_ = sample.sampled_at_us;
            ++opposite_peak_samples_;
            opposite_strength_ = std::max(opposite_strength_, tracked_strength);
            settle_samples_ = 0;
        } else if (opposite_peak_samples_ > 0 && tracked_strength < kRotationSettleDps) {
            ++settle_samples_;
        }
    }

    if (shake_peaks_ >= 4 &&
        sample.sampled_at_us - shake_window_started_us_ <= kShakeWindowUs) {
        return emit(GestureKind::shake, acceleration_delta, sample.sampled_at_us);
    }
    if (raise_armed_ && raise_confirm_samples_ >= 3) {
        return emit(GestureKind::raise, -sample.accel_z_g, sample.sampled_at_us);
    }
    if (rotation_active_ && settle_samples_ >= 3) {
        const auto total = sample.sampled_at_us - rotation_started_us_;
        const auto brake_delay = opposite_started_us_ - rotation_first_ended_us_;
        const bool flick = rotation_strength_ >= profile_.flick_peak_dps &&
                           opposite_strength_ >= kFlickBrakeDps &&
                           brake_delay <= kFlickBrakeWindowUs &&
                           opposite_peak_samples_ <= 2 &&
                           rotation_accel_delta_ >= 0.12F &&
                           rotation_accel_delta_ <= 0.80F && total <= 400'000;
        if (flick) return emit(GestureKind::flick, rotation_strength_, sample.sampled_at_us);
        const bool twist = rotation_strength_ >= profile_.twist_peak_dps &&
                           opposite_peak_samples_ >= 2 &&
                           opposite_strength_ >= profile_.twist_peak_dps &&
                           brake_delay >= 120'000 && total >= 320'000 &&
                           total <= kTwistWindowUs;
        if (twist) {
            return emit(GestureKind::double_twist,
                        std::max(rotation_strength_, opposite_strength_),
                        sample.sampled_at_us);
        }
        reset_transient_state();
    } else if (rotation_active_ &&
               sample.sampled_at_us - rotation_started_us_ > kTwistWindowUs) {
        reset_transient_state();
    }
    return output;
}

GestureProcessorOutput GestureProcessor::emit(GestureKind kind, float strength,
                                              std::int64_t now_us) noexcept {
    switch (kind) {
        case GestureKind::raise: cooldown_until_us_ = now_us + kRaiseCooldownUs; break;
        case GestureKind::double_twist: cooldown_until_us_ = now_us + kTwistCooldownUs; break;
        case GestureKind::shake: cooldown_until_us_ = now_us + kShakeCooldownUs; break;
        case GestureKind::flick: cooldown_until_us_ = now_us + kFlickCooldownUs; break;
        case GestureKind::none: break;
    }
    reset_transient_state();
    return {.sample_valid = true, .detected = kind, .strength = strength};
}

void GestureProcessor::reset_transient_state() noexcept {
    raise_armed_ = false;
    raise_arm_samples_ = 0;
    raise_rotation_samples_ = 0;
    raise_confirm_samples_ = 0;
    raise_rotation_seen_ = false;
    shake_armed_ = true;
    shake_peaks_ = 0;
    shake_axis_ = 0;
    shake_sign_ = 0;
    shake_window_started_us_ = 0;
    last_shake_peak_us_ = 0;
    rotation_active_ = false;
    rotation_settled_ = false;
    rotation_peak_samples_ = 0;
    opposite_peak_samples_ = 0;
    settle_samples_ = 0;
    rotation_strength_ = 0.0F;
    opposite_strength_ = 0.0F;
    rotation_accel_delta_ = 0.0F;
    rotation_started_us_ = 0;
    rotation_first_ended_us_ = 0;
    opposite_started_us_ = 0;
}

void GestureProcessor::reset() noexcept {
    const auto retained_profile = profile_;
    *this = GestureProcessor(retained_profile);
}

const char *gesture_name(GestureKind kind) noexcept {
    switch (kind) {
        case GestureKind::raise: return "RAISE";
        case GestureKind::double_twist: return "DOUBLE TWIST";
        case GestureKind::shake: return "SHAKE";
        case GestureKind::flick: return "FLICK";
        case GestureKind::none: break;
    }
    return "NONE";
}

}  // namespace nightglass::services
