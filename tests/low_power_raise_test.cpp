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
    LowPowerRaise raise;
    for (int i=1; i<20; ++i) assert(!raise.process(1,0,0,i*100'000,0.7,true));
    assert(!raise.process(3,0,-1,2'000'000,0.7,true)); // Impact disarms.
    assert(!raise.process(0,0,-1,2'100'000,0.7,true));
    assert(!raise.process(std::numeric_limits<float>::quiet_NaN(),0,0,2'200'000,0.7,true));
    assert(!raise.process(0,0,-1,2'100'000,0.7,true)); // Out-of-order sample.
    assert(!raise.process(0,0,-1,4'000'000,0.7,true)); // Sensor gap.
}
