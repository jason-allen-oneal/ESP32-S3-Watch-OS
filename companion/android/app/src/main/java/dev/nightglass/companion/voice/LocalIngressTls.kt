package dev.nightglass.companion.voice

import okhttp3.OkHttpClient
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

/** Verify the USB-enrolled server certificate before sending any HTTP credentials.
 * OkHttp's default hostname verifier remains enabled (the certificate has the LAN IP SAN).
 */
internal object LocalIngressTls {
    fun trustManager(fingerprint: String): X509TrustManager {
        require(fingerprint.matches(Regex("[a-f0-9]{64}")))
        return object : X509TrustManager {
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            override fun checkClientTrusted(chain: Array<X509Certificate>, authType: String) {
                throw CertificateException("Client trust is not used")
            }
            override fun checkServerTrusted(chain: Array<X509Certificate>, authType: String) {
                val certificate = chain.firstOrNull() ?: throw CertificateException("Missing server certificate")
                certificate.checkValidity()
                val actual = MessageDigest.getInstance("SHA-256").digest(certificate.encoded)
                val expected = fingerprint.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
                if (!MessageDigest.isEqual(actual, expected)) throw CertificateException("Local server certificate mismatch")
            }
        }
    }

    fun client(base: OkHttpClient, fingerprint: String): OkHttpClient {
        val trust = trustManager(fingerprint)
        val tls = SSLContext.getInstance("TLS")
        tls.init(null, arrayOf(trust), null)
        return base.newBuilder().sslSocketFactory(tls.socketFactory, trust)
            .followRedirects(false).followSslRedirects(false).build()
    }
}
