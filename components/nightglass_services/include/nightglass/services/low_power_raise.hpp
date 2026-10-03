#pragma once

#include <cmath>
#include <cstdint>

namespace nightglass::services {

// Accel-only tilt recognition for raise-only configurations. Gyro-dependent
// twist/shake/flick and guided calibration retain the existing gyro pipeline.
class LowPowerRaise {
public:
    bool process(float x, float y, float z, std::int64_t now, float face_up_g,
                 bool enabled) noexcept {
        const float magnitude = std::sqrt(x * x + y * y + z * z);
        if (!enabled || now <= last_us_ || !std::isfinite(magnitude) ||
            magnitude < 0.35F || magnitude > 2.5F ||
            (last_us_ > 0 && now - last_us_ > 500'000)) {
            reset_pose();
            last_us_ = now;
            return false;
        }
        last_us_ = now;
        if (now < cooldown_us_) { reset_pose(); return false; }
        // A normal lift briefly departs from 1g. Preserve its down-pose
        // baseline, but never count unsettled samples as a face-up finish.
        if (magnitude < 0.75F || magnitude > 1.25F) {
            settled_us_ = 0;
            previous_z_ = z;
            return false;
        }
        if (!armed_) {
            if (z >= -face_up_g + 0.25F) {
                if (arm_started_us_ == 0) arm_started_us_ = now;
                if (now - arm_started_us_ >= 300'000) { armed_ = true; start_z_ = z; }
            } else { arm_started_us_ = 0; }
            return false;
        }
        if (candidate_us_ == 0 && start_z_ - z >= 0.25F) candidate_us_ = now;
        if (candidate_us_ > 0 && now - candidate_us_ > 2'000'000) {
            reset_pose();
            return false;
        }
        // Require a real change of pose plus a settled, gravity-like finish.
        // A resting face-up watch cannot wake itself; large impacts reset it.
        if (candidate_us_ > 0 && z <= -face_up_g && std::fabs(z - previous_z_) < 0.06F) {
            if (settled_us_ == 0) settled_us_ = now;
            if (now - settled_us_ >= 250'000) {
                cooldown_us_ = now + 2'500'000;
                reset_pose();
                return true;
            }
        } else { settled_us_ = 0; }
        previous_z_ = z;
        return false;
    }

private:
    void reset_pose() noexcept {
        armed_ = false;
        arm_started_us_ = 0;
        candidate_us_ = settled_us_ = 0;
        start_z_ = previous_z_ = 0;
    }
    bool armed_{false};
    std::int64_t arm_started_us_{0};
    float start_z_{0}, previous_z_{0};
    std::int64_t last_us_{0}, candidate_us_{0}, settled_us_{0}, cooldown_us_{0};
};

} // namespace nightglass::services
