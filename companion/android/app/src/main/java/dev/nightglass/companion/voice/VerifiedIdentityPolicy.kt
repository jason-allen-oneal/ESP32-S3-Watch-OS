package dev.nightglass.companion.voice

import java.net.URI

/** Gateway tokens are omitted for identity ingress; the pinned local origin uses
 * its own separately enrolled, revocable phone credential at the TLS proxy.
 */
internal object VerifiedIdentityPolicy {
    const val LOCAL_ENDPOINT = "wss://192.168.10.185:18792/nightglass/ws"
    const val REMOTE_ENDPOINT = "wss://rev.tailfa9b46.ts.net:8443/nightglass/ws"
    fun isPhoneIngressEndpoint(url: String): Boolean = isLocalEndpoint(url) || url == REMOTE_ENDPOINT
    fun isLocalEndpoint(url: String): Boolean = url == LOCAL_ENDPOINT
    fun isEndpoint(url: String): Boolean = runCatching {
        val uri = URI(url)
        isPhoneIngressEndpoint(url) || (uri.scheme == "wss" && uri.host == "rev.tailfa9b46.ts.net" &&
            uri.port in setOf(-1, 443) && uri.path == "/nightglass/ws" &&
            uri.userInfo == null && uri.rawQuery == null && uri.fragment == null)
    }.getOrDefault(false)

    fun grantsVoice(scopes: Collection<String>, identityAuth: Boolean): Boolean =
        OpenClawVoiceStore.scopesAreExactlyRequired(scopes) ||
            (identityAuth && ("operator.admin" in scopes || "operator.write" in scopes))
}
