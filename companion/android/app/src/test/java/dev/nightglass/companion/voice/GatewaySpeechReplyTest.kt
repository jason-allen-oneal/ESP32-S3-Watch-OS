package dev.nightglass.companion.voice

import java.util.Base64
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.*
import org.junit.Test

class GatewaySpeechReplyTest {
    private fun reply(bytes: ByteArray, format: String = GatewaySpeechReply.FORMAT,
                      provider: String = "elevenlabs") = buildJsonObject {
        put("provider", provider)
        put("outputFormat", format)
        put("audioBase64", Base64.getEncoder().encodeToString(bytes))
    }
    @Test fun acceptsExactWatchFormat() {
        val audio = byteArrayOf(0, 1, -1, 127)
        assertArrayEquals(audio, GatewaySpeechReply.decode(reply(audio)))
    }
    @Test fun rejectsContainerAndWrongProvider() {
        for (value in listOf(reply(byteArrayOf(1), "mp3"),
                             reply(byteArrayOf(1), provider = "openai"))) {
            assertThrows(IllegalArgumentException::class.java) { GatewaySpeechReply.decode(value) }
        }
    }
    @Test fun rejectsEmptyAndOversizedAudio() {
        for (audio in listOf(byteArrayOf(), ByteArray(96_001))) {
            assertThrows(IllegalArgumentException::class.java) { GatewaySpeechReply.decode(reply(audio)) }
        }
    }
    @Test fun retainsShortTextAndBoundsLongSpeech() {
        assertEquals("Hello world.", GatewaySpeechReply.spokenExcerpt(" Hello world. "))
        val full = "a longer reply ".repeat(100)
        val spoken = GatewaySpeechReply.spokenExcerpt(full)
        assertTrue(spoken.length <= GatewaySpeechReply.MAX_TEXT_CHARS)
        assertTrue(full.startsWith(spoken))
    }
}
