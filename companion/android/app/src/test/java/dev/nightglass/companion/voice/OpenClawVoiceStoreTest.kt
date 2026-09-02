package dev.nightglass.companion.voice

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OpenClawVoiceStoreTest {
    private fun credential(bootstrap: String, url: String = "https://gateway.invalid") =
        OpenClawVoiceCredential(
            url = url,
            bootstrapToken = bootstrap,
            operatorToken = null,
            scopes = emptySet(),
            tlsFingerprint = null,
            deviceId = "device",
            publicKeyRaw = byteArrayOf(1, 2, 3),
            privateKeyPkcs8 = byteArrayOf(4, 5, 6),
            sessionKey = null,
        )

    @Test fun staleHandoffCannotReplaceNewerProvisioning() {
        val inFlight = credential("older-bootstrap")
        val current = credential("newer-bootstrap")

        assertFalse(OpenClawVoiceStore.sameProvisioning(current, inFlight))
        assertTrue(OpenClawVoiceStore.sameProvisioning(current, current.copy()))
    }

    @Test fun staleScopeUpgradeCannotReplaceANewerOperatorCredential() {
        val inFlight = credential("bootstrap").copy(
            bootstrapToken = null,
            operatorToken = "old-operator-token",
            scopes = OpenClawVoiceStore.BOOTSTRAP_SCOPES,
        )
        val current = inFlight.copy(
            operatorToken = "new-operator-token",
            scopes = OpenClawVoiceStore.REQUIRED_SCOPES,
        )

        assertFalse(OpenClawVoiceStore.sameProvisioning(current, inFlight))
        assertTrue(OpenClawVoiceStore.scopesAreAllowedHandoff(inFlight.scopes))
        assertTrue(OpenClawVoiceStore.scopesAreExactlyRequired(current.scopes))
        assertFalse(OpenClawVoiceStore.scopesAreAllowedHandoff(
            current.scopes + "operator.admin"))
    }

    @Test fun validatesAndCompareAndSwapsOnlyNightglassSessionPointers() {
        val prefix = OpenClawVoiceStore.SESSION_KEY_PREFIX
        val first = prefix + "123e4567-e89b-12d3-a456-426614174000"
        val second = prefix + "123e4567-e89b-12d3-a456-426614174001"
        assertTrue(OpenClawVoiceStore.isNightglassSessionKey(first))
        assertFalse(OpenClawVoiceStore.isNightglassSessionKey("agent:main:dashboard:abc"))
        assertFalse(OpenClawVoiceStore.isNightglassSessionKey(prefix))
        assertFalse(OpenClawVoiceStore.isNightglassSessionKey(prefix + "bad key"))
        assertFalse(OpenClawVoiceStore.isNightglassSessionKey(prefix + "abc-123"))
        assertFalse(OpenClawVoiceStore.isNightglassSessionKey(
            prefix + "123E4567-E89B-12D3-A456-426614174000"))

        val current = credential("bootstrap").copy(sessionKey = first)
        assertTrue(OpenClawVoiceStore.canReplaceSessionKey(current, current.copy(), first))
        assertFalse(OpenClawVoiceStore.canReplaceSessionKey(current, current.copy(), null))
        assertFalse(OpenClawVoiceStore.canReplaceSessionKey(
            current, current.copy(sessionKey = second), first))

        val reprovisioned = current.copy(bootstrapToken = "new-bootstrap", sessionKey = null)
        assertFalse(OpenClawVoiceStore.canReplaceSessionKey(
            reprovisioned, current, first))
        assertTrue(reprovisioned.sessionKey == null)
        assertTrue(current.sessionKey == first)
    }
}
