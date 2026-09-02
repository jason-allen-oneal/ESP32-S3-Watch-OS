package dev.nightglass.companion.voice

object VoiceAudioCodec {
    const val MAX_MULAW_WAV_BYTES = 2_400_000
    const val WAV_HEADER_BYTES = 44

    fun decodeMulaw(sample: Byte): Short {
        val encoded = sample.toInt().inv() and 0xff
        val sign = encoded and 0x80
        val exponent = (encoded ushr 4) and 0x07
        val mantissa = encoded and 0x0f
        val magnitude = (((mantissa shl 3) + 0x84) shl exponent) - 0x84
        return (if (sign != 0) -magnitude else magnitude)
            .coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt()).toShort()
    }

    /**
     * Wrap one complete bounded watch turn as standard 8 kHz mono PCM16 WAV.
     * Keeping the source rate avoids tripling the attachment before the
     * Gateway host's whisper.cpp input stage.
     */
    fun mulaw8kToWav(input: ByteArray): ByteArray {
        require(input.size in 1..MAX_MULAW_WAV_BYTES)
        val dataBytes = Math.multiplyExact(input.size, 2)
        val output = ByteArray(Math.addExact(WAV_HEADER_BYTES, dataBytes))
        output.writeAscii(0, "RIFF")
        output.writeLe32(4, 36 + dataBytes)
        output.writeAscii(8, "WAVE")
        output.writeAscii(12, "fmt ")
        output.writeLe32(16, 16)
        output.writeLe16(20, 1)
        output.writeLe16(22, 1)
        output.writeLe32(24, 8_000)
        output.writeLe32(28, 16_000)
        output.writeLe16(32, 2)
        output.writeLe16(34, 16)
        output.writeAscii(36, "data")
        output.writeLe32(40, dataBytes)
        var out = WAV_HEADER_BYTES
        input.forEach { encoded ->
            val sample = decodeMulaw(encoded).toInt()
            output[out++] = (sample and 0xff).toByte()
            output[out++] = ((sample ushr 8) and 0xff).toByte()
        }
        return output
    }

    private fun ByteArray.writeAscii(offset: Int, value: String) {
        value.forEachIndexed { index, char -> this[offset + index] = char.code.toByte() }
    }

    private fun ByteArray.writeLe16(offset: Int, value: Int) {
        this[offset] = (value and 0xff).toByte()
        this[offset + 1] = ((value ushr 8) and 0xff).toByte()
    }

    private fun ByteArray.writeLe32(offset: Int, value: Int) {
        this[offset] = (value and 0xff).toByte()
        this[offset + 1] = ((value ushr 8) and 0xff).toByte()
        this[offset + 2] = ((value ushr 16) and 0xff).toByte()
        this[offset + 3] = ((value ushr 24) and 0xff).toByte()
    }
}
