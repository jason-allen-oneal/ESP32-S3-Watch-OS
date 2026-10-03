package dev.nightglass.companion.voice

import org.junit.Assert.*
import org.junit.Test
import kotlin.math.abs

class VoiceReplyGainTest {
    @Test fun quietSpeechGetsApproximatelySixDbWithoutChangingLength() {
        val audio = intArrayOf(0, 1000, -1000, 5000, -5000).map {
            VoiceAudioCodec.encodeMulaw(it)
        }.toByteArray()
        val before = audio.map { VoiceAudioCodec.decodeMulaw(it).toInt() }
        VoiceAudioCodec.boostSpokenReply(audio)
        assertEquals(before.size, audio.size)
        before.indices.forEach { i ->
            val after = VoiceAudioCodec.decodeMulaw(audio[i]).toInt()
            assertTrue(abs(after - before[i] * 2) <= 256)
        }
    }
    @Test fun allEncodedInputsStayMonotonicSymmetricAndBelowClipping() {
        val input = ByteArray(256) { it.toByte() }
        val before = input.map { VoiceAudioCodec.decodeMulaw(it).toInt() }
        VoiceAudioCodec.boostSpokenReply(input)
        val pairs = before.zip(input.map { VoiceAudioCodec.decodeMulaw(it).toInt() }).sortedBy { it.first }
        pairs.forEach { (old, boosted) ->
            assertTrue(abs(boosted) <= 32124)
            assertTrue(abs(boosted) >= abs(old) || abs(old) > 30000)
            assertTrue(old == 0 && boosted == 0 || old.toLong() * boosted > 0)
        }
        pairs.zipWithNext().forEach { (a, b) -> assertTrue(a.second <= b.second) }
        for (i in 0..127) assertEquals(-VoiceAudioCodec.decodeMulaw(input[i]).toInt(),
            VoiceAudioCodec.decodeMulaw(input[i + 128]).toInt())
    }
}
