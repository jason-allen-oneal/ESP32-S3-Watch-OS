package dev.nightglass.companion.voice

import android.content.Context
import android.util.Log
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
        var credential = store.load()
            ?: return OpenClawProbeResult(configured = false, reachable = false, fatal = false)
        try {
            val hasBootstrap = !credential.bootstrapToken.isNullOrBlank()
            val hasOperator = !credential.operatorToken.isNullOrBlank()
            if ((!hasBootstrap && !hasOperator) || !internetAvailable) {
                return OpenClawProbeResult(
                    configured = hasBootstrap || hasOperator,
                    reachable = false,
                    fatal = false,
                )
            }

            // Complete the one-time voice-node handoff during the health probe.
            // Provisioning should become healthy before the user records audio;
            // a first utterance is not an acceptable pairing side effect.
            if (!hasOperator) {
                val bootstrap = credential.bootstrapToken
                    ?: return OpenClawProbeResult(false, reachable = false, fatal = false)
                var bootstrapSocket: OpenClawVoiceGateway.RpcSocket? = null
                try {
                    bootstrapSocket = OpenClawVoiceGateway.RpcSocket(
                        credential,
                        store,
                        "node",
                        emptyList(),
                        bootstrap,
                        true,
                    )
                    val hello = bootstrapSocket.connect()
                    val handoff = OpenClawVoiceGateway.parseOperatorHandoff(hello)
                    if (!store.persistOperatorToken(credential, handoff.token, handoff.scopes)) {
                        return OpenClawProbeResult(true, reachable = false, fatal = false)
                    }
                } catch (error: OpenClawVoiceGateway.GatewayConnectRejectedException) {
                    logFailure("bootstrap_rejected", error)
                    return OpenClawProbeResult(true, reachable = false, fatal = true)
                } catch (error: SecurityException) {
                    logFailure("bootstrap_security", error)
                    return OpenClawProbeResult(true, reachable = false, fatal = true)
                } catch (error: IllegalArgumentException) {
                    logFailure("bootstrap_contract", error)
                    return OpenClawProbeResult(true, reachable = false, fatal = true)
                } catch (error: Throwable) {
                    logFailure("bootstrap_transport", error)
                    return OpenClawProbeResult(true, reachable = false, fatal = false)
                } finally {
                    bootstrapSocket?.close()
                }
                credential.wipe()
                credential = store.load()
                    ?: return OpenClawProbeResult(true, reachable = false, fatal = true)
            }

            val configured = credential.operatorToken != null &&
                OpenClawVoiceStore.scopesAreExactlyRequired(credential.scopes)
            if (!configured) return OpenClawProbeResult(true, reachable = false, fatal = true)
            val token = credential.operatorToken
                ?: return OpenClawProbeResult(true, reachable = false, fatal = true)
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
            } catch (error: OpenClawVoiceGateway.GatewayConnectRejectedException) {
                logFailure("operator_rejected", error)
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (error: SecurityException) {
                logFailure("operator_security", error)
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (error: IllegalArgumentException) {
                logFailure("operator_contract", error)
                OpenClawProbeResult(true, reachable = false, fatal = true)
            } catch (error: Throwable) {
                logFailure("operator_transport", error)
                OpenClawProbeResult(true, reachable = false, fatal = false)
            } finally {
                socket?.close()
            }
        } finally {
            credential.wipe()
        }
    }

    private fun logFailure(stage: String, error: Throwable) {
        // Never log the exception message: network errors may embed the private
        // endpoint, while Gateway errors may carry authentication details.
        val classes = generateSequence(error) { it.cause }
            .take(4)
            .joinToString(">") { it.javaClass.simpleName }
        Log.i(TAG, "OpenClaw health failure: stage=$stage classes=$classes")
    }

    fun close() = Unit

    private companion object { const val TAG = "NightglassLink" }
}
