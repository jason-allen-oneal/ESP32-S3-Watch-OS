package dev.nightglass.companion.voice

import dev.nightglass.companion.protocol.NightglassProtocol
import java.util.Base64
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive

/** The installed watch accepts raw mono 8 kHz mu-law, not MP3/WAV containers. */
object GatewaySpeechReply {
    const val MODEL = "eleven_flash_v2_5"
    const val FORMAT = "ulaw_8000"
    const val MAX_TEXT_CHARS = 140

    fun spokenExcerpt(text: String): String {
        val clean = text.trim()
        if (clean.length <= MAX_TEXT_CHARS) return clean
        val end = clean.lastIndexOf(' ', MAX_TEXT_CHARS).takeIf { it >= 70 }
            ?: MAX_TEXT_CHARS
        return clean.substring(0, end).trim()
    }

    fun decode(reply: JsonObject): ByteArray {
        require(reply["provider"]?.jsonPrimitive?.content == "elevenlabs") {
            "Watch speech requires ElevenLabs"
        }
        require(reply["outputFormat"]?.jsonPrimitive?.content == FORMAT) {
            "Watch speech requires raw 8 kHz mu-law"
        }
        val encoded = reply["audioBase64"]?.jsonPrimitive?.content
            ?: error("Speech response has no audio")
        val maxBytes = NightglassProtocol.MAX_SPOKEN_REPLY_BYTES
        require(encoded.isNotEmpty() && encoded.length <= ((maxBytes + 2) / 3) * 4) {
            "Speech exceeds the watch's 12-second playback limit"
        }
        val audio = Base64.getDecoder().decode(encoded)
        if (audio.isEmpty() || audio.size > maxBytes) {
            audio.fill(0)
            error("Speech exceeds the watch playback limit")
        }
        return audio
    }
}
