#include "nightglass/services/gyro_processor.hpp"

#include <algorithm>
#include <cmath>

namespace nightglass::services {
namespace {

constexpr float kCalibrationAccelMinSquared = 0.90F * 0.90F;
constexpr float kCalibrationAccelMaxSquared = 1.10F * 1.10F;
constexpr float kCalibrationGyroMaxSquared = 6.0F * 6.0F;
constexpr float kCalibrationDeltaMaxSquared = 1.5F * 1.5F;
constexpr float kCalibrationStdDevMaxDps = 0.8F;

bool finite_sample(const GyroSample &sample) {
    return std::isfinite(sample.x_dps) && std::isfinite(sample.y_dps) &&
           std::isfinite(sample.z_dps) && std::isfinite(sample.accel_x_g) &&
           std::isfinite(sample.accel_y_g) && std::isfinite(sample.accel_z_g);
}

bool stationary_sample(const GyroSample &sample) {
    if (!finite_sample(sample)) return false;
    const float accel_energy = sample.accel_x_g * sample.accel_x_g +
                               sample.accel_y_g * sample.accel_y_g +
                               sample.accel_z_g * sample.accel_z_g;
    const float gyro_energy = sample.x_dps * sample.x_dps + sample.y_dps * sample.y_dps +
                              sample.z_dps * sample.z_dps;
    return accel_energy >= kCalibrationAccelMinSquared &&
           accel_energy <= kCalibrationAccelMaxSquared &&
           gyro_energy <= kCalibrationGyroMaxSquared;
}

float variance(double sum, double sum_sq, std::uint16_t count) {
    const double mean = sum / count;
    return static_cast<float>(std::max(0.0, sum_sq / count - mean * mean));
}

float deadband(float value) {
    return std::fabs(value) <= GyroProcessor::kDisplayDeadbandDps ? 0.0F : value;
}

}  // namespace

GyroOutput GyroProcessor::process(const GyroSample &sample) {
    // A failed read is skipped by hardware.cpp; this guard ensures any future
    // invalid conversion also cannot advance or destroy a partial calibration.
    if (!finite_sample(sample)) return output();

    if (!calibrated_) {
        if (warmup_samples_ < kWarmupSamples) {
            ++warmup_samples_;
            return output();
        }
        if (!stationary_sample(sample)) {
            const bool had_samples = calibration_samples_ != 0;
            restart_calibration();
            return output(had_samples);
        }

        if (previous_sample_valid_) {
            const float delta_x = sample.x_dps - previous_x_dps_;
            const float delta_y = sample.y_dps - previous_y_dps_;
            const float delta_z = sample.z_dps - previous_z_dps_;
            const float delta_energy = delta_x * delta_x + delta_y * delta_y +
                                       delta_z * delta_z;
            if (delta_energy > kCalibrationDeltaMaxSquared) {
                const bool had_samples = calibration_samples_ != 0;
                restart_calibration();
                return output(had_samples);
            }
        }

        ++calibration_samples_;
        previous_x_dps_ = sample.x_dps;
        previous_y_dps_ = sample.y_dps;
        previous_z_dps_ = sample.z_dps;
        previous_sample_valid_ = true;
        sum_x_ += sample.x_dps;
        sum_y_ += sample.y_dps;
        sum_z_ += sample.z_dps;
        sum_sq_x_ += static_cast<double>(sample.x_dps) * sample.x_dps;
        sum_sq_y_ += static_cast<double>(sample.y_dps) * sample.y_dps;
        sum_sq_z_ += static_cast<double>(sample.z_dps) * sample.z_dps;

        if (calibration_samples_ == kRequiredSamples) {
            const float stddev_x = std::sqrt(variance(sum_x_, sum_sq_x_, calibration_samples_));
            const float stddev_y = std::sqrt(variance(sum_y_, sum_sq_y_, calibration_samples_));
            const float stddev_z = std::sqrt(variance(sum_z_, sum_sq_z_, calibration_samples_));
            if (stddev_x > kCalibrationStdDevMaxDps ||
                stddev_y > kCalibrationStdDevMaxDps ||
                stddev_z > kCalibrationStdDevMaxDps) {
                restart_calibration();
                return output(true);
            }

            bias_x_dps_ = static_cast<float>(sum_x_ / calibration_samples_);
            bias_y_dps_ = static_cast<float>(sum_y_ / calibration_samples_);
            bias_z_dps_ = static_cast<float>(sum_z_ / calibration_samples_);
            filtered_x_dps_ = 0.0F;
            filtered_y_dps_ = 0.0F;
            filtered_z_dps_ = 0.0F;
            corrected_x_dps_ = 0.0F;
            corrected_y_dps_ = 0.0F;
            corrected_z_dps_ = 0.0F;
            calibrated_ = true;
        }
        return output();
    }

    corrected_x_dps_ = sample.x_dps - bias_x_dps_;
    corrected_y_dps_ = sample.y_dps - bias_y_dps_;
    corrected_z_dps_ = sample.z_dps - bias_z_dps_;
    filtered_x_dps_ += kFilterAlpha * (corrected_x_dps_ - filtered_x_dps_);
    filtered_y_dps_ += kFilterAlpha * (corrected_y_dps_ - filtered_y_dps_);
    filtered_z_dps_ += kFilterAlpha * (corrected_z_dps_ - filtered_z_dps_);
    return output();
}

void GyroProcessor::reset() {
    calibration_restarts_ = 0;
    calibrated_ = false;
    bias_x_dps_ = 0.0F;
    bias_y_dps_ = 0.0F;
    bias_z_dps_ = 0.0F;
    filtered_x_dps_ = 0.0F;
    filtered_y_dps_ = 0.0F;
    filtered_z_dps_ = 0.0F;
    corrected_x_dps_ = 0.0F;
    corrected_y_dps_ = 0.0F;
    corrected_z_dps_ = 0.0F;
    calibration_samples_ = 0;
    warmup_samples_ = 0;
    previous_sample_valid_ = false;
    sum_x_ = sum_y_ = sum_z_ = 0.0;
    sum_sq_x_ = sum_sq_y_ = sum_sq_z_ = 0.0;
}

void GyroProcessor::restart_calibration() {
    if (calibration_samples_ != 0) ++calibration_restarts_;
    calibration_samples_ = 0;
    previous_sample_valid_ = false;
    sum_x_ = sum_y_ = sum_z_ = 0.0;
    sum_sq_x_ = sum_sq_y_ = sum_sq_z_ = 0.0;
}

GyroOutput GyroProcessor::output(bool restarted) const {
    return {
        .calibrated = calibrated_,
        .calibration_restarted = restarted,
        .calibration_samples = calibration_samples_,
        .calibration_required = kRequiredSamples,
        .calibration_restarts = calibration_restarts_,
        .bias_x_dps = bias_x_dps_,
        .bias_y_dps = bias_y_dps_,
        .bias_z_dps = bias_z_dps_,
        .corrected_x_dps = corrected_x_dps_,
        .corrected_y_dps = corrected_y_dps_,
        .corrected_z_dps = corrected_z_dps_,
        .filtered_x_dps = filtered_x_dps_,
        .filtered_y_dps = filtered_y_dps_,
        .filtered_z_dps = filtered_z_dps_,
        .display_x_dps = deadband(filtered_x_dps_),
        .display_y_dps = deadband(filtered_y_dps_),
        .display_z_dps = deadband(filtered_z_dps_),
    };
}

}  // namespace nightglass::services
