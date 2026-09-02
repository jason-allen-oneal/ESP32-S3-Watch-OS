package dev.nightglass.companion.voice

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class OpenClawOperatorHandoffTest {
    @Test fun usesAStockGatewayClientIdentity() {
        assertEquals("openclaw-android", OpenClawVoiceGateway.CLIENT_ID)
    }

    private fun hello(scopes: String) = Json.parseToJsonElement(
        """{"auth":{"deviceTokens":[{"role":"operator","deviceToken":"abcdefghijklmnop","scopes":$scopes}]}}"""
    ).jsonObject

    @Test fun acceptsTheStockVoiceBootstrapAndApprovedWriteHandoffs() {
        val bootstrap = OpenClawVoiceGateway.parseOperatorHandoff(
            hello("""["operator.read","operator.talk"]"""))
        assertEquals("abcdefghijklmnop", bootstrap.token)
        assertEquals(OpenClawVoiceStore.BOOTSTRAP_SCOPES, bootstrap.scopes)

        val approved = OpenClawVoiceGateway.parseOperatorHandoff(
            hello("""["operator.read","operator.talk","operator.write"]"""))
        assertEquals(OpenClawVoiceStore.REQUIRED_SCOPES, approved.scopes)
    }

    @Test fun acceptsDirectConstrainedOperatorBootstrap() {
        val direct = Json.parseToJsonElement("""{
          "auth":{"role":"operator","deviceToken":"abcdefghijklmnop",
          "scopes":["operator.read","operator.talk"]}
        }""").jsonObject
        val bootstrap = OpenClawVoiceGateway.parseOperatorHandoff(direct)
        assertEquals("abcdefghijklmnop", bootstrap.token)
        assertEquals(OpenClawVoiceStore.BOOTSTRAP_SCOPES, bootstrap.scopes)
    }

    @Test fun rejectsBroaderOrIncompleteHandoffs() {
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseOperatorHandoff(
                hello("""["operator.read","operator.talk","operator.write","operator.admin"]"""))
        }
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseOperatorHandoff(
                hello("""["operator.talk"]"""))
        }
    }

    @Test fun acceptsOnlyAnApprovedClosedScopeUpgradeResult() {
        val approved = Json.parseToJsonElement("""{
          "status":"approved","requestId":"request-1",
          "deviceToken":"abcdefghijklmnop",
          "scopes":["operator.read","operator.talk","operator.write"]
        }""").jsonObject
        assertEquals(OpenClawVoiceStore.REQUIRED_SCOPES,
            OpenClawVoiceGateway.parseApprovedScopeUpgrade(approved, "request-1").scopes)

        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseApprovedScopeUpgrade(approved, "different-request")
        }

        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseApprovedScopeUpgrade(
                Json.parseToJsonElement("""{
                  "status":"approved","requestId":"request-2",
                  "deviceToken":"abcdefghijklmnop",
                  "scopes":["operator.read","operator.talk","operator.write","operator.admin"]
                }""").jsonObject, "request-2")
        }
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.parseApprovedScopeUpgrade(
                Json.parseToJsonElement("""{
                  "status":"rejected","requestId":"request-3"
                }""").jsonObject, "request-3")
        }
    }

    @Test fun rejectsExpiredMissingAndMalformedScopeUpgradeResults() {
        listOf(
            """{"status":"expired","requestId":"request-4"}""",
            """{"status":"approved","requestId":"request-4",
                "deviceToken":"abcdefghijklmnop"}""",
            """{"status":"approved","requestId":"request-4",
                "deviceToken":"short",
                "scopes":["operator.read","operator.talk","operator.write"]}""",
            """{"status":"approved",
                "deviceToken":"abcdefghijklmnop",
                "scopes":["operator.read","operator.talk","operator.write"]}""",
        ).forEach { payload ->
            assertThrows(IllegalArgumentException::class.java) {
                OpenClawVoiceGateway.parseApprovedScopeUpgrade(
                    Json.parseToJsonElement(payload).jsonObject, "request-4")
            }
        }
    }
}
