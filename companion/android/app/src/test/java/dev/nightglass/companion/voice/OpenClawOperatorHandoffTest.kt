package dev.nightglass.companion.voice

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class OpenClawOperatorHandoffTest {
    @Test fun usesTheDedicatedVoiceRelayIdentity() {
        assertEquals("openclaw-voice-relay", OpenClawVoiceGateway.CLIENT_ID)
    }

    private fun hello(scopes: String) = Json.parseToJsonElement(
        """{"auth":{"deviceTokens":[{"role":"operator","deviceToken":"abcdefghijklmnop","scopes":$scopes}]}}"""
    ).jsonObject

    @Test fun acceptsOnlyTheConstrainedVoiceOperatorHandoff() {
        val handoff = OpenClawVoiceGateway.parseOperatorHandoff(
            hello("""["operator.read","operator.talk"]"""))
        assertEquals("abcdefghijklmnop", handoff.token)
        assertEquals(setOf("operator.read", "operator.talk"), handoff.scopes)
    }

    @Test fun rejectsBroaderOrIncompleteHandoffs() {
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseOperatorHandoff(
                hello("""["operator.read","operator.talk","operator.write"]"""))
        }
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseOperatorHandoff(
                hello("""["operator.talk"]"""))
        }
    }
}
