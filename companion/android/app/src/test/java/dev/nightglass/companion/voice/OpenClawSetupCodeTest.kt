package dev.nightglass.companion.voice

import java.util.Base64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class OpenClawSetupCodeTest {
    private fun code(json: String) = Base64.getUrlEncoder().withoutPadding()
        .encodeToString(json.toByteArray())

    @Test fun acceptsBoundedVoiceNodeSetup() {
        val parsed = OpenClawSetupCode.decode(code(
            """{"url":"wss://voice.example.test","bootstrapToken":"abcdefghijklmnop","expiresAtMs":999999}"""), 1)
        assertEquals("wss://voice.example.test", parsed.url)
        assertEquals("abcdefghijklmnop", parsed.bootstrapToken)
    }

    @Test fun acceptsLoopbackOnlyCleartext() {
        assertEquals("ws://127.0.0.1:18789", OpenClawSetupCode.decode(code(
            """{"url":"ws://127.0.0.1:18789","bootstrapToken":"abcdefghijklmnop"}""")).url)
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawSetupCode.decode(code(
                """{"url":"ws://192.168.1.2:18789","bootstrapToken":"abcdefghijklmnop"}"""))
        }
    }

    @Test fun rejectsExpiredOrMalformedSetup() {
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawSetupCode.decode(code(
                """{"url":"wss://voice.example.test","bootstrapToken":"abcdefghijklmnop","expiresAtMs":5}"""), 5)
        }
        assertThrows(IllegalArgumentException::class.java) { OpenClawSetupCode.decode("not-json") }
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawSetupCode.decode(code(
                """{"url":"wss://voice.example.test/?token=forbidden","bootstrapToken":"abcdefghijklmnop"}"""))
        }
    }

    @Test fun validatesTlsFingerprintAndExactVoiceScopes() {
        val fingerprint = "ab".repeat(32)
        val parsed = OpenClawSetupCode.decode(code(
            """{"url":"wss://voice.example.test","bootstrapToken":"abcdefghijklmnop","tlsFingerprint":"$fingerprint"}"""))
        assertEquals(fingerprint, parsed.tlsFingerprint)
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawSetupCode.decode(code(
                """{"url":"wss://voice.example.test","bootstrapToken":"abcdefghijklmnop","tlsFingerprint":"${"gg".repeat(32)}"}"""))
        }
        assertTrue(OpenClawVoiceStore.scopesAreExactlyRequired(
            listOf("operator.talk", "operator.read")))
        assertFalse(OpenClawVoiceStore.scopesAreExactlyRequired(
            listOf("operator.talk", "operator.read", "operator.write")))
        assertFalse(OpenClawVoiceStore.scopesAreExactlyRequired(listOf("operator.talk")))
    }
}
