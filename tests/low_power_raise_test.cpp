#include "nightglass/services/low_power_raise.hpp"
#include <cassert>
#include <algorithm>
#include <initializer_list>
#include <cmath>
#include <limits>

using nightglass::services::LowPowerRaise;

int main() {
    for (const auto dt : {40'000, 100'000}) {
        LowPowerRaise raise;
        std::int64_t now = 0;
        auto sample = [&](float z, bool enabled = true) {
            now += dt;
            return raise.process(std::sqrt(std::max(0.0F, 1-z*z)), 0, z,
                                 now, 0.70F, enabled);
        };
        for (int i=0; i<100; ++i) assert(!sample(-1)); // Stationary face up.
        for (int i=0; i<100; ++i) assert(!sample(0)); // Arm down, no movement.
        for (const auto z : {-0.2F, -0.4F, -0.6F, -0.8F}) assert(!sample(z));
        int wakes = 0;
        for (int i=0; i<20; ++i) wakes += sample(-0.9F);
        assert(wakes==1); // One settled tilt, not repeated face-up wake.
        for (int i=0; i<100; ++i) assert(!sample(-0.9F));
        for (int i=0; i<10; ++i) assert(!sample(0));
        assert(!sample(-0.8F));
        assert(!sample(0)); // Abandon finish; a stale candidate must expire.
        for (int i=0; i<60; ++i) assert(!sample(0));
        for (int i=0; i<10; ++i) assert(!sample(-0.9F, false));
        for (int i=0; i<100; ++i) assert(!sample(-0.9F)); // Re-enable face-up.
    }
    // Standby scheduling jitter and ordinary lift acceleration retain the
    // baseline, but an actual impact and stale multi-second gap still reset.
    for (const auto dt : {100'000, 180'000, 250'000}) {
        LowPowerRaise jitter;
        std::int64_t now = 0;
        for (int i=0; i<10; ++i) {
            now += dt;
            assert(!jitter.process(1,0,0,now,0.7,true));
        }
        now += dt;
        assert(!jitter.process(1.3,0,-0.5,now,0.7,true));
        int wakes = 0;
        for (int i=0; i<12; ++i) {
            now += dt;
            wakes += jitter.process(0.4359,0,-0.9,now,0.7,true);
        }
        assert(wakes == 1);
    }
    LowPowerRaise raise;
    for (int i=1; i<20; ++i) assert(!raise.process(1,0,0,i*100'000,0.7,true));
    assert(!raise.process(3,0,-1,2'000'000,0.7,true)); // Impact disarms.
    assert(!raise.process(0,0,-1,2'100'000,0.7,true));
    assert(!raise.process(std::numeric_limits<float>::quiet_NaN(),0,0,2'200'000,0.7,true));
    assert(!raise.process(0,0,-1,2'100'000,0.7,true)); // Out-of-order sample.
    assert(!raise.process(0,0,-1,4'000'000,0.7,true)); // Sensor gap.
}
