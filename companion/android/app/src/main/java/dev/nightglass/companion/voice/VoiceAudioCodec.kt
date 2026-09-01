package dev.nightglass.companion.voice

object VoiceAudioCodec {
    // Five seconds of mu-law expands to 240,000 raw PCM bytes and 320,000
    // base64 characters, safely below Gateway's 512 KiB append limit.
    const val MAX_MULAW_CHUNK_BYTES = 40_000
    const val MAX_PCM_CHUNK_BYTES = MAX_MULAW_CHUNK_BYTES * 6

    fun decodeMulaw(sample: Byte): Short {
        val encoded = sample.toInt().inv() and 0xff
        val sign = encoded and 0x80
        val exponent = (encoded ushr 4) and 0x07
        val mantissa = encoded and 0x0f
        val magnitude = (((mantissa shl 3) + 0x84) shl exponent) - 0x84
        return (if (sign != 0) -magnitude else magnitude)
            .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
    }

    // Gateway Talk currently consumes 24 kHz PCM16. Each 8 kHz mu-law sample
    // expands to three identical PCM samples. This bounded zero-order hold is
    // sufficient for telephone-band speech and introduces no persistent file.
    fun mulaw8kToPcm24k(input: ByteArray): ByteArray {
        require(input.size <= MAX_MULAW_CHUNK_BYTES)
        return mulaw8kChunkToPcm24k(input, 0, input.size)
    }

    /** Decode at most five seconds without allocating whole-turn PCM. */
    fun mulaw8kChunkToPcm24k(input: ByteArray, offset: Int, length: Int): ByteArray {
        require(offset >= 0 && length in 1..MAX_MULAW_CHUNK_BYTES &&
            offset <= input.size - length)
        val output = ByteArray(length * 6)
        var out = 0
        for (index in offset until offset + length) {
            val value = input[index]
            val sample = decodeMulaw(value).toInt()
            repeat(3) {
                output[out++] = (sample and 0xff).toByte()
                output[out++] = ((sample ushr 8) and 0xff).toByte()
            }
        }
        return output
    }
}
