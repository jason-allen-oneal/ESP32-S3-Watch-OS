package dev.nightglass.companion.voice

import java.net.URI
import java.util.Base64
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

data class OpenClawSetup(
    val url: String,
    val bootstrapToken: String,
    val expiresAtMs: Long?,
    val tlsFingerprint: String?,
)

object OpenClawSetupCode {
    private const val PREFIX = "oc-pair://"
    private val json = Json { ignoreUnknownKeys = true }

    fun decode(input: CharSequence, nowMs: Long = System.currentTimeMillis()): OpenClawSetup {
        val raw = input.toString().trim().let {
            if (it.startsWith(PREFIX, ignoreCase = true)) it.substring(PREFIX.length) else it
        }
        require(raw.length in 32..8192 && raw.all { it.isLetterOrDigit() || it == '-' || it == '_' })
        val decoded = Base64.getUrlDecoder().decode(raw)
        require(decoded.size in 32..6144)
        val root = json.parseToJsonElement(decoded.toString(Charsets.UTF_8)).jsonObject
        decoded.fill(0)
        val url = root["url"]?.jsonPrimitive?.content?.trim().orEmpty()
        val token = root["bootstrapToken"]?.jsonPrimitive?.content?.trim().orEmpty()
        require(token.length in 16..4096)
        validateUrl(url)
        val expires = root["expiresAtMs"]?.jsonPrimitive?.content?.toLongOrNull()
        require(expires == null || expires > nowMs)
        val fingerprint = root["tlsFingerprint"]?.jsonPrimitive?.content
            ?.filterNot { it == ':' }
            ?.lowercase()
            ?.takeIf { value -> value.length == 64 && value.all { it in '0'..'9' || it in 'a'..'f' } }
        if (root.containsKey("tlsFingerprint")) require(fingerprint != null)
        return OpenClawSetup(url, token, expires, fingerprint)
    }

    private fun validateUrl(value: String) {
        require(value.length in 8..2048)
        val uri = URI(value)
        require(uri.userInfo == null && uri.fragment == null && uri.rawQuery == null &&
            !uri.host.isNullOrBlank())
        if (uri.scheme.equals("wss", ignoreCase = true)) return
        require(uri.scheme.equals("ws", ignoreCase = true))
        require(uri.host.equals("127.0.0.1") || uri.host.equals("localhost", ignoreCase = true) || uri.host == "::1")
    }
}
