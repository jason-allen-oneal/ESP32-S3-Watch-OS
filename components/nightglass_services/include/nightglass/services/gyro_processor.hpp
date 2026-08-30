#pragma once

#include <cstdint>

namespace nightglass::services {

struct GyroSample {
    float x_dps{0.0F};
    float y_dps{0.0F};
    float z_dps{0.0F};
    float accel_x_g{0.0F};
    float accel_y_g{0.0F};
    float accel_z_g{0.0F};
};

struct GyroOutput {
    bool calibrated{false};
    bool calibration_restarted{false};
    std::uint16_t calibration_samples{0};
    std::uint16_t calibration_required{0};
    std::uint32_t calibration_restarts{0};
    float bias_x_dps{0.0F};
    float bias_y_dps{0.0F};
    float bias_z_dps{0.0F};
    float corrected_x_dps{0.0F};
    float corrected_y_dps{0.0F};
    float corrected_z_dps{0.0F};
    float filtered_x_dps{0.0F};
    float filtered_y_dps{0.0F};
    float filtered_z_dps{0.0F};
    float display_x_dps{0.0F};
    float display_y_dps{0.0F};
    float display_z_dps{0.0F};
};

// Runtime zero-rate calibration and conditioning for the QMI8658 gyro.
// The processor is dependency-free so its math can be tested on the host.
class GyroProcessor {
public:
    static constexpr std::uint16_t kWarmupSamples = 25;
    static constexpr std::uint16_t kRequiredSamples = 500;
    static constexpr float kFilterAlpha = 0.25F;
    static constexpr float kDisplayDeadbandDps = 0.5F;

    GyroOutput process(const GyroSample &sample);
    void reset();

private:
    void restart_calibration();
    [[nodiscard]] GyroOutput output(bool restarted = false) const;

    std::uint16_t calibration_samples_{0};
    std::uint16_t warmup_samples_{0};
    std::uint32_t calibration_restarts_{0};
    double sum_x_{0.0};
    double sum_y_{0.0};
    double sum_z_{0.0};
    double sum_sq_x_{0.0};
    double sum_sq_y_{0.0};
    double sum_sq_z_{0.0};
    float bias_x_dps_{0.0F};
    float bias_y_dps_{0.0F};
    float bias_z_dps_{0.0F};
    float corrected_x_dps_{0.0F};
    float corrected_y_dps_{0.0F};
    float corrected_z_dps_{0.0F};
    float filtered_x_dps_{0.0F};
    float filtered_y_dps_{0.0F};
    float filtered_z_dps_{0.0F};
    float previous_x_dps_{0.0F};
    float previous_y_dps_{0.0F};
    float previous_z_dps_{0.0F};
    bool previous_sample_valid_{false};
    bool calibrated_{false};
};

}  // namespace nightglass::services
