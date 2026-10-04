package dev.nightglass.companion.voice
import org.junit.Assert.*
import org.junit.Test
class VerifiedIdentityPolicyTest {
    @Test fun publicPhoneCredentialIsBoundToExactTlsRelay() {
        val endpoint = VerifiedIdentityPolicy.REMOTE_ENDPOINT
        assertTrue(VerifiedIdentityPolicy.isPhoneIngressEndpoint(endpoint))
        assertTrue(VerifiedIdentityPolicy.isEndpoint(endpoint))
        for (url in listOf(endpoint + "?redirect=evil", endpoint + "/", endpoint.replace("8443", "10000"),
            endpoint.replace("wss:", "ws:"), endpoint.replace(".ts.net", ".ts.net.evil.example"))) {
            assertFalse(url, VerifiedIdentityPolicy.isPhoneIngressEndpoint(url))
            assertFalse(url, VerifiedIdentityPolicy.isEndpoint(url))
        }
        assertFalse(VerifiedIdentityPolicy.isPhoneIngressEndpoint("wss://rev.tailfa9b46.ts.net/nightglass/ws"))
    }

    @Test fun localCredentialIsBoundToOneTlsOriginAndPath() {
        val endpoint = VerifiedIdentityPolicy.LOCAL_ENDPOINT
        assertTrue(VerifiedIdentityPolicy.isLocalEndpoint(endpoint))
        assertTrue(VerifiedIdentityPolicy.isEndpoint(endpoint))
        for (url in listOf(endpoint.replace("wss:", "ws:"), endpoint + "?redirect=evil",
            endpoint + "#fragment", endpoint.replace("18792", "18793"),
            endpoint.replace("192.168.10.185", "192.168.10.249"), endpoint + "/")) {
            assertFalse(url, VerifiedIdentityPolicy.isLocalEndpoint(url))
            assertFalse(url, VerifiedIdentityPolicy.isEndpoint(url))
        }
    }
    @Test fun localTlsRefusesMissingCertificateAndMalformedPin() {
        assertThrows(IllegalArgumentException::class.java) { LocalIngressTls.trustManager("bad") }
        val trust = LocalIngressTls.trustManager("a".repeat(64))
        assertThrows(java.security.cert.CertificateException::class.java) {
            trust.checkServerTrusted(emptyArray(), "RSA")
        }
        assertThrows(java.security.cert.CertificateException::class.java) {
            trust.checkClientTrusted(emptyArray(), "RSA")
        }
    }
    @Test fun localTlsAcceptsOnlyTheEnrolledCertificate() {
        val input = requireNotNull(javaClass.getResourceAsStream("/local-ingress-test.crt"))
        val certificate = input.use {
            java.security.cert.CertificateFactory.getInstance("X.509").generateCertificate(it)
        } as java.security.cert.X509Certificate
        val pin = java.security.MessageDigest.getInstance("SHA-256").digest(certificate.encoded)
            .joinToString("") { "%02x".format(it) }
        LocalIngressTls.trustManager(pin).checkServerTrusted(arrayOf(certificate), "RSA")
        assertThrows(java.security.cert.CertificateException::class.java) {
            LocalIngressTls.trustManager("0".repeat(64)).checkServerTrusted(arrayOf(certificate), "RSA")
        }
    }
    @Test fun onlyExactTlsIdentityIngressCanOmitCredentials() {
        assertTrue(VerifiedIdentityPolicy.isEndpoint("wss://rev.tailfa9b46.ts.net/nightglass/ws"))
        for (url in listOf("ws://rev.tailfa9b46.ts.net/nightglass/ws",
            "wss://rev.tailfa9b46.ts.net.evil.example/nightglass/ws",
            "wss://rev.tailfa9b46.ts.net/", "wss://rev.tailfa9b46.ts.net:9443/nightglass/ws",
            "wss://user@rev.tailfa9b46.ts.net/nightglass/ws"))
            assertFalse(url, VerifiedIdentityPolicy.isEndpoint(url))
    }
    @Test fun broadHumanGrantsNeverRelaxDeviceTokenScopeContract() {
        assertTrue(VerifiedIdentityPolicy.grantsVoice(setOf("operator.admin"), true))
        assertTrue(VerifiedIdentityPolicy.grantsVoice(setOf("operator.write"), true))
        assertFalse(VerifiedIdentityPolicy.grantsVoice(setOf("operator.admin"), false))
        assertFalse(VerifiedIdentityPolicy.grantsVoice(setOf("operator.read", "operator.talk"), true))
    }
}
