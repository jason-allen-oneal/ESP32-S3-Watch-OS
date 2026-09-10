#pragma once

#include <cstdint>
#include <type_traits>

#include "nightglass/services/gesture_processor.hpp"

namespace nightglass::services {

enum class GestureCalibrationStage : std::uint8_t {
    idle,
    stationary,
    raise,
    double_twist,
    shake,
    flick,
    quiet,
    complete,
};

enum class GestureCapturePhase : std::uint8_t {
    waiting,
    countdown,
    recording,
};

enum class GestureCalibrationResult : std::uint8_t {
    none,
    sample_ok,
    sample_missed,
    quiet_retry,
    profile_saved,
};

struct GestureCalibrationSnapshot {
    GestureCalibrationStage stage{GestureCalibrationStage::idle};
    GestureCapturePhase phase{GestureCapturePhase::waiting};
    GestureCalibrationResult last_result{GestureCalibrationResult::none};
    std::uint8_t repetitions{0};
    std::uint8_t repetitions_required{3};
    std::uint16_t attempts{0};
    std::uint16_t quiet_false_positives{0};
    std::uint32_t remaining_ms{0};
    bool completion_pending{false};
};

static_assert(std::is_trivially_copyable_v<GestureCalibrationSnapshot>);

// A deterministic, allocation-free calibration state machine. The user tells
// it which gesture is intentional by starting a bounded capture window. A
// labelled feature extractor measures three successful examples without
// requiring the stricter everyday recognizer to classify its own training
// input; a final clean quiet window rejects or tightens hair-trigger profiles.
class GestureCalibrator {
public:
    static constexpr std::uint8_t kRequiredRepetitions = 3;
    static constexpr std::int64_t kCountdownUs = 1'500'000;
    static constexpr std::int64_t kGestureCaptureUs = 4'000'000;
    static constexpr std::int64_t kRaiseCaptureUs = 5'000'000;
    static constexpr std::int64_t kQuietCaptureUs = 15'000'000;

    void start(std::int64_t now_us) noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool request_capture(std::int64_t now_us) noexcept;
    void process(const GestureSample &sample) noexcept;
    [[nodiscard]] GestureCalibrationSnapshot snapshot(std::int64_t now_us) const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const GestureProfile &candidate_profile() const noexcept {
        return candidate_profile_;
    }
    bool take_completed_profile(GestureProfile &profile) noexcept;

private:
    void begin_recording(std::int64_t now_us) noexcept;
    void reset_capture() noexcept;
    void process_labelled_capture(const GestureSample &sample) noexcept;
    void accept_sample(float strength, float gyro_strength) noexcept;
    void advance_stage() noexcept;
    void tune_from_false_positive(GestureKind kind, float strength) noexcept;

    GestureCalibrationStage stage_{GestureCalibrationStage::idle};
    GestureCapturePhase phase_{GestureCapturePhase::waiting};
    GestureCalibrationResult last_result_{GestureCalibrationResult::none};
    GestureProcessor quiet_processor_{};
    GestureProfile candidate_profile_{};
    std::int64_t deadline_us_{0};
    std::uint8_t repetitions_{0};
    std::uint16_t attempts_{0};
    std::uint16_t quiet_false_positives_{0};
    float minimum_strength_{0.0F};
    float minimum_gyro_strength_{0.0F};
    float capture_gyro_strength_{0.0F};
    float capture_peak_strength_{0.0F};
    float capture_opposite_strength_{0.0F};
    float capture_acceleration_strength_{0.0F};
    std::int64_t capture_first_peak_us_{0};
    std::int64_t capture_last_peak_us_{0};
    std::uint8_t capture_axis_{0};
    std::int8_t capture_sign_{0};
    std::uint8_t capture_peak_samples_{0};
    std::uint8_t capture_settle_samples_{0};
    std::uint8_t capture_opposite_samples_{0};
    std::uint8_t capture_shake_peaks_{0};
    bool capture_motion_seen_{false};
    bool capture_motion_settled_{false};
    bool capture_shake_armed_{true};
    bool quiet_window_dirty_{false};
    bool completion_pending_{false};
};

[[nodiscard]] bool gesture_calibration_active(
    const GestureCalibrationSnapshot &snapshot) noexcept;
[[nodiscard]] const char *gesture_calibration_stage_name(
    GestureCalibrationStage stage) noexcept;

}  // namespace nightglass::services
