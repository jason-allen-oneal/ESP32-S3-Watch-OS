package dev.nightglass.companion.voice

import android.content.Context
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

enum class OpenClawHealth(val wireValue: Int) {
    UNAVAILABLE(0),
    DEGRADED(1),
    HEALTHY(2),
}

data class OpenClawProbeResult(
    val configured: Boolean,
    val reachable: Boolean,
    val fatal: Boolean,
)

data class OpenClawHealthDecision(
    val state: OpenClawHealth,
    val consecutiveFailures: Int,
)

object OpenClawHealthPolicy {
    fun decide(result: OpenClawProbeResult, internetAvailable: Boolean,
               previousFailures: Int): OpenClawHealthDecision {
        if (!result.configured || !internetAvailable) {
            return OpenClawHealthDecision(OpenClawHealth.DEGRADED, 0)
        }
        if (result.reachable) return OpenClawHealthDecision(OpenClawHealth.HEALTHY, 0)
        val failures = (previousFailures + 1).coerceAtMost(2)
        return OpenClawHealthDecision(
            if (result.fatal || failures >= 2) OpenClawHealth.UNAVAILABLE
            else OpenClawHealth.DEGRADED,
            failures,
        )
    }
}

/** Authenticated Gateway capability check. It opens no Talk/AI session. */
class OpenClawHealthProbe(context: Context) {
    private val store = OpenClawVoiceStore(context.applicationContext)

    fun check(internetAvailable: Boolean): OpenClawProbeResult {
        val credential = store.load()
            ?: return OpenClawProbeResult(configured = false, reachable = false, fatal = false)
        try {
            val configured = credential.operatorToken != null &&
                OpenClawVoiceStore.scopesAreExactlyRequired(credential.scopes)
            if (!configured || !internetAvailable) {
                return OpenClawProbeResult(configured, reachable = false, fatal = false)
            }
            val token = credential.operatorToken
                ?: return OpenClawProbeResult(false, reachable = false, fatal = false)
            var socket: OpenClawVoiceGateway.RpcSocket? = null
            return try {
                socket = OpenClawVoiceGateway.RpcSocket(
                    credential,
                    store,
                    "operator",
                    OpenClawVoiceStore.REQUIRED_SCOPES.sorted(),
                    token,
                    false,
                )
                val hello = socket.connect()
                require(hello["auth"]?.jsonObject?.get("role")?.jsonPrimitive?.content ==
                    "operator")
                val scopes = hello["auth"]?.jsonObject?.get("scopes")?.jsonArray
                    ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
                require(OpenClawVoiceStore.scopesAreExactlyRequired(scopes))
                val methods = hello["features"]?.jsonObject?.get("methods")?.jsonArray
                    ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
                require("talk.session.create" in methods &&
                    "talk.session.appendAudio" in methods)
                OpenClawProbeResult(true, reachable = true, fatal = false)
            } catch (_: OpenClawVoiceGateway.GatewayConnectRejectedException) {
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (_: SecurityException) {
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (_: IllegalArgumentException) {
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (_: Throwable) {
                OpenClawProbeResult(true, reachable = false, fatal = false)
            } finally {
                socket?.close()
            }
        } finally {
            credential.wipe()
        }
    }

    fun close() = Unit
}
