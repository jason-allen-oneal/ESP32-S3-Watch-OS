#pragma once

#include <cstdint>
#include <type_traits>

namespace nightglass::services {

enum class GestureKind : std::uint8_t {
    none,
    raise,
    double_twist,
    shake,
    flick,
};

// A small, versioned sensitivity profile learned by the guided on-watch
// calibration flow. These are deliberately scalar gates rather than an
// opaque model: every learned value remains bounded, inspectable, and safe to
// fall back from when persisted data is missing or malformed.
struct GestureProfile {
    static constexpr std::uint32_t kFormatVersion = 1;

    std::uint32_t format_version{kFormatVersion};
    float raise_face_up_g{0.70F};
    float twist_peak_dps{100.0F};
    float shake_peak_g{0.35F};
    float shake_gyro_dps{70.0F};
    float flick_peak_dps{180.0F};
    std::uint8_t calibrated{0};
    std::uint8_t reserved[3]{};
};

static_assert(std::is_trivially_copyable_v<GestureProfile>);

[[nodiscard]] GestureProfile default_gesture_profile() noexcept;
[[nodiscard]] GestureProfile training_gesture_profile() noexcept;
[[nodiscard]] bool valid_gesture_profile(const GestureProfile &profile) noexcept;

struct GestureSample {
    float accel_x_g{0.0F};
    float accel_y_g{0.0F};
    float accel_z_g{0.0F};
    float gyro_x_dps{0.0F};
    float gyro_y_dps{0.0F};
    float gyro_z_dps{0.0F};
    std::int64_t sampled_at_us{0};
    bool allow_raise{false};
    bool gyro_calibrated{true};
};

struct GestureProcessorOutput {
    bool sample_valid{false};
    GestureKind detected{GestureKind::none};
    float strength{0.0F};
};

// Bounded, allocation-free gesture recognizer for the 25 Hz QMI8658 stream.
// It deliberately recognizes only high-confidence macro gestures. Cooldowns,
// timing windows, and settle gates prevent one physical movement from being
// emitted as several UI actions.
class GestureProcessor {
public:
    explicit GestureProcessor(
        const GestureProfile &profile = default_gesture_profile()) noexcept;
    GestureProcessorOutput process(const GestureSample &sample) noexcept;
    void reset() noexcept;
    void set_profile(const GestureProfile &profile) noexcept;
    [[nodiscard]] const GestureProfile &profile() const noexcept { return profile_; }

private:
    GestureProcessorOutput emit(GestureKind kind, float strength,
                                std::int64_t now_us) noexcept;
    void reset_transient_state() noexcept;

    std::int64_t last_sample_us_{0};
    std::int64_t cooldown_until_us_{0};

    bool raise_armed_{false};
    std::uint8_t raise_arm_samples_{0};
    std::uint8_t raise_rotation_samples_{0};
    std::uint8_t raise_confirm_samples_{0};
    std::int64_t raise_armed_at_us_{0};
    bool raise_rotation_seen_{false};

    bool shake_armed_{true};
    std::uint8_t shake_peaks_{0};
    std::uint8_t shake_axis_{0};
    std::int8_t shake_sign_{0};
    std::int64_t shake_window_started_us_{0};
    std::int64_t last_shake_peak_us_{0};

    bool rotation_active_{false};
    bool rotation_settled_{false};
    std::uint8_t rotation_axis_{0};
    std::int8_t rotation_sign_{0};
    std::uint8_t rotation_peak_samples_{0};
    std::uint8_t opposite_peak_samples_{0};
    std::uint8_t settle_samples_{0};
    float rotation_strength_{0.0F};
    float opposite_strength_{0.0F};
    float rotation_accel_delta_{0.0F};
    std::int64_t rotation_started_us_{0};
    std::int64_t rotation_first_ended_us_{0};
    std::int64_t opposite_started_us_{0};
    GestureProfile profile_{};
};

[[nodiscard]] const char *gesture_name(GestureKind kind) noexcept;

}  // namespace nightglass::services
