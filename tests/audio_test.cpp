#include <cassert>
#include <cstdint>

#include "nightglass/services/audio.hpp"

int main() {
    using nightglass::services::bounded_audio_bytes;
    using nightglass::services::pcm16_rms;
    const std::int16_t samples[] = {-3, 4, 0, 0};
    assert(bounded_audio_bytes(0) == 0);
    assert(bounded_audio_bytes(4096) == 4096);
    assert(bounded_audio_bytes(4097) == 4096);
    assert(pcm16_rms(samples, 4) == 2);
    assert(pcm16_rms(nullptr, 4) == 0);
    return 0;
}
