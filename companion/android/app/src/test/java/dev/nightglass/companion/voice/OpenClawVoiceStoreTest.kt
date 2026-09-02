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
        )

    @Test fun staleHandoffCannotReplaceNewerProvisioning() {
        val inFlight = credential("older-bootstrap")
        val current = credential("newer-bootstrap")

        assertFalse(OpenClawVoiceStore.sameProvisioning(current, inFlight))
        assertTrue(OpenClawVoiceStore.sameProvisioning(current, current.copy()))
    }
}
