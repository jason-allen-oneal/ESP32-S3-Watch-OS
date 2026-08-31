#include <cassert>
#include <cstdint>

#include "nightglass/services/audio.hpp"

int main() {
    using nightglass::services::bounded_audio_bytes;
    using nightglass::services::pcm16_rms;
    using nightglass::services::SoundCue;
    using nightglass::services::sound_cue_duration_ms;
    using nightglass::services::sound_cue_transfer_bytes;
    using nightglass::services::parse_pcm16_mono_wav;
    using nightglass::services::AudioSettings;
    using nightglass::services::valid_audio_settings;
    using nightglass::services::audio_cue_allowed;
    const std::int16_t samples[] = {-3, 4, 0, 0};
    assert(bounded_audio_bytes(0) == 0);
    assert(bounded_audio_bytes(3) == 2);
    assert(bounded_audio_bytes(7) == 6);
    assert(bounded_audio_bytes(4096) == 4096);
    assert(bounded_audio_bytes(4097) == 4096);
    assert(pcm16_rms(samples, 4) == 2);
    assert(pcm16_rms(nullptr, 4) == 0);
    static_assert(sound_cue_duration_ms(SoundCue::notification) == 420);
    static_assert(sound_cue_transfer_bytes(SoundCue::test) == 64000);
    static_assert(sound_cue_transfer_bytes(SoundCue::timer) == 38400);
    static_assert(valid_audio_settings(AudioSettings{100, false, true}));
    static_assert(!valid_audio_settings(AudioSettings{101, false, false}));
    static_assert(!audio_cue_allowed(SoundCue::notification,
                                     AudioSettings{60, false, true}));
    static_assert(audio_cue_allowed(SoundCue::alarm, AudioSettings{60, false, true}));
    static_assert(!audio_cue_allowed(SoundCue::alarm, AudioSettings{60, true, false}));

    std::array<std::uint8_t, 48> wav{
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
        0x80, 0x3e, 0, 0, 0x00, 0x7d, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0, 1, 0, 2, 0};
    const auto parsed = parse_pcm16_mono_wav(wav);
    assert(parsed.valid && parsed.data_offset == 44 && parsed.data_size == 4);
    auto malformed = wav;
    malformed[32] = 4;  // contradictory mono PCM block alignment
    assert(!parse_pcm16_mono_wav(malformed).valid);
    malformed = wav;
    malformed[29] = 0;  // contradictory byte rate
    assert(!parse_pcm16_mono_wav(malformed).valid);
    malformed = wav;
    malformed[4] = 39;  // RIFF container size does not cover the file
    assert(!parse_pcm16_mono_wav(malformed).valid);
    return 0;
}
