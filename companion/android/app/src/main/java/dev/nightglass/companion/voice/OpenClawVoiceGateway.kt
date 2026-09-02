package dev.nightglass.companion.voice

import android.content.Context
import android.os.SystemClock
import dev.nightglass.companion.protocol.NightglassProtocol
import java.security.MessageDigest
import java.security.cert.X509Certificate
import java.util.Base64
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener

class OpenClawVoiceGateway(
    context: Context,
    private val status: (String) -> Unit,
) {
    private val store = OpenClawVoiceStore(context.applicationContext)
    private val executor = Executors.newSingleThreadExecutor()
    private val activeLock = Any()
    @Volatile private var active: ActiveTurn? = null

    companion object {
        internal const val CLIENT_ID = "openclaw-voice-relay"

        internal data class OperatorHandoff(
            val token: String,
            val scopes: Set<String>,
        )

        fun acceptsEncodedBytes(size: Int): Boolean =
            size in 1..NightglassProtocol.MAX_VOICE_ENCODED_BYTES

        fun responseTimeoutSeconds(encodedBytes: Int): Long {
            require(acceptsEncodedBytes(encodedBytes))
            val capturedSeconds = (encodedBytes + 7_999L) / 8_000L
            return (capturedSeconds + 60L).coerceIn(60L, 360L)
        }

        internal fun parseOperatorHandoff(hello: JsonObject): OperatorHandoff {
            val handoff = hello["auth"]?.jsonObject?.get("deviceTokens")?.jsonArray
                ?.mapNotNull { it as? JsonObject }
                ?.firstOrNull { it["role"]?.jsonPrimitive?.content == "operator" }
                ?: error("Gateway did not issue a constrained operator handoff")
            val scopes = handoff["scopes"]?.jsonArray
                ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
            val token = handoff["deviceToken"]?.jsonPrimitive?.content.orEmpty()
            require(OpenClawVoiceStore.scopesAreExactlyRequired(scopes) && token.isNotBlank())
            return OperatorHandoff(token, scopes)
        }
    }

    private class ActiveTurn(val owner: VoiceTurnOwner) {
        val cancelled = AtomicBoolean(false)
        val socket = AtomicReference<RpcSocket?>(null)
        val relaySession = AtomicReference<String?>(null)
        val eventFence = VoiceEventFence(owner)
        @Volatile var worker: FutureTask<Unit>? = null

        fun cancel() {
            if (!cancelled.compareAndSet(false, true)) return
            eventFence.cancel()
            socket.getAndSet(null)?.abort(relaySession.get())
            worker?.cancel(true)
        }

        fun checkActive() {
            if (cancelled.get() || Thread.currentThread().isInterrupted) {
                throw CancellationException("OpenClaw voice turn cancelled")
            }
        }
    }

    fun provision(setupCode: CharSequence) {
        val decoded = OpenClawSetupCode.decode(setupCode)
        store.provision(decoded)
        status("OpenClaw voice setup saved; first turn will complete pairing")
    }

    fun clear() {
        cancel()
        store.clear()
        status("Local OpenClaw voice authorization cleared; revoke this device in OpenClaw separately")
    }

    fun configured(): Boolean = store.load()?.let { credential ->
        credential.wipe()
        true
    } ?: false

    fun submit(
        owner: VoiceTurnOwner,
        encoded: ByteArray,
        completed: (VoiceTurnOwner, Result<String>) -> Unit,
    ): Boolean {
        if (!acceptsEncodedBytes(encoded.size)) return false
        val owned = encoded.copyOf()
        val turn = ActiveTurn(owner)
        synchronized(activeLock) {
            if (active != null) {
                owned.fill(0)
                return false
            }
            active = turn
        }
        val task = FutureTask<Unit> {
            val result = try { runCatching { runTurn(turn, owned) } }
            finally { owned.fill(0) }
            val deliver = synchronized(activeLock) {
                if (active === turn && !turn.cancelled.get()) {
                    active = null
                    true
                } else false
            }
            if (deliver) completed(owner, result)
        }
        turn.worker = task
        executor.execute(task)
        return true
    }

    fun cancel(owner: VoiceTurnOwner? = null) {
        val turn = synchronized(activeLock) {
            val current = active
            if (current == null || (owner != null && current.owner != owner)) null
            else current.also { active = null }
        }
        turn?.cancel()
    }

    fun close() {
        cancel()
        executor.shutdownNow()
    }

    private fun runTurn(turn: ActiveTurn, mulaw: ByteArray): String {
        var credential = store.load() ?: error("OpenClaw voice is not configured")
        try {
            turn.checkActive()
            if (credential.operatorToken == null) {
                status("Pairing constrained OpenClaw voice identity")
                val bootstrap = credential.bootstrapToken ?: error("OpenClaw setup code expired or was consumed")
                val socket = RpcSocket(credential, store, "node", emptyList(), bootstrap, true)
                turn.socket.set(socket)
                val hello = try { socket.connect() } finally {
                    turn.socket.compareAndSet(socket, null)
                    socket.close()
                }
                turn.checkActive()
                val handoff = parseOperatorHandoff(hello)
                check(store.persistOperatorToken(credential, handoff.token, handoff.scopes)) {
                    "OpenClaw setup changed during authorization; try again"
                }
                credential.wipe()
                credential = store.load() ?: error("OpenClaw voice authorization was not persisted")
            }

            status("Connecting to OpenClaw")
            val assistant = CompletableFuture<String>()
            val socket = RpcSocket(
                credential, store, "operator", OpenClawVoiceStore.REQUIRED_SCOPES.sorted(),
                credential.operatorToken ?: error("OpenClaw voice authorization missing"), false,
            ) { event, payload, sequence ->
                if (event != "talk.event") return@RpcSocket
                val session = payload["relaySessionId"]?.jsonPrimitive?.content
                    ?: payload["sessionId"]?.jsonPrimitive?.content
                val eventTurn = payload["turnId"]?.jsonPrimitive?.content
                when (turn.eventFence.accept(turn.owner, session, eventTurn, sequence)) {
                    VoiceEventFence.Decision.DROP -> return@RpcSocket
                    VoiceEventFence.Decision.GAP -> {
                        assistant.completeExceptionally(SecurityException(
                            "OpenClaw event sequence gap"))
                        return@RpcSocket
                    }
                    VoiceEventFence.Decision.ACCEPT -> Unit
                }
                when (payload["type"]?.jsonPrimitive?.content) {
                    "transcript" -> if (
                        payload["role"]?.jsonPrimitive?.content == "assistant" &&
                        payload["final"]?.jsonPrimitive?.content == "true"
                    ) {
                        if (!eventTurn.isNullOrBlank()) {
                            payload["text"]?.jsonPrimitive?.content?.let(assistant::complete)
                        }
                    }
                    "error" -> assistant.completeExceptionally(
                        IllegalStateException(payload["message"]?.jsonPrimitive?.content
                            ?: "OpenClaw Talk failed"))
                    "close" -> if (!assistant.isDone) assistant.completeExceptionally(
                        IllegalStateException("OpenClaw Talk closed before a response"))
                }
            }
            turn.socket.set(socket)
            try {
                val hello = socket.connect()
                turn.checkActive()
                require(hello["auth"]?.jsonObject?.get("role")?.jsonPrimitive?.content == "operator")
                val scopes = hello["auth"]?.jsonObject?.get("scopes")?.jsonArray
                    ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
                require(OpenClawVoiceStore.scopesAreExactlyRequired(scopes))
                val methods = hello["features"]?.jsonObject?.get("methods")?.jsonArray
                    ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
                require("talk.session.create" in methods && "talk.session.appendAudio" in methods)

                status("Opening OpenClaw voice session")
                val created = socket.request("talk.session.create", buildJsonObject {
                    put("mode", "realtime")
                    put("transport", "gateway-relay")
                    put("brain", "agent-consult")
                }, 20_000)
                val session = created["relaySessionId"]?.jsonPrimitive?.content
                    ?: created["sessionId"]?.jsonPrimitive?.content
                    ?: error("OpenClaw did not create a relay session")
                val audio = created["audio"]?.jsonObject
                    ?: error("OpenClaw did not declare an audio contract")
                require(audio["inputEncoding"]?.jsonPrimitive?.content == "pcm16" &&
                    audio["inputSampleRateHz"]?.jsonPrimitive?.content?.toIntOrNull() == 24_000)
                turn.relaySession.set(session)
                turn.eventFence.bindRelay(session)
                turn.checkActive()
                status("Sending voice to OpenClaw")
                var encodedOffset = 0
                while (encodedOffset < mulaw.size) {
                    turn.checkActive()
                    val length = minOf(VoiceAudioCodec.MAX_MULAW_CHUNK_BYTES,
                        mulaw.size - encodedOffset)
                    val chunk = VoiceAudioCodec.mulaw8kChunkToPcm24k(
                        mulaw, encodedOffset, length)
                    try {
                        socket.request("talk.session.appendAudio", buildJsonObject {
                            put("sessionId", session)
                            put("audioBase64", Base64.getEncoder().encodeToString(chunk))
                            put("timestamp", SystemClock.elapsedRealtime())
                        }, 20_000)
                    } finally { chunk.fill(0) }
                    encodedOffset += length
                }
                val silence = ByteArray(48_000)
                try {
                    socket.request("talk.session.appendAudio", buildJsonObject {
                        put("sessionId", session)
                        put("audioBase64", Base64.getEncoder().encodeToString(silence))
                        put("timestamp", SystemClock.elapsedRealtime())
                    }, 10_000)
                } finally { silence.fill(0) }
                turn.checkActive()
                status("OpenClaw is thinking")
                val response = assistant.get(responseTimeoutSeconds(mulaw.size), TimeUnit.SECONDS)
                turn.checkActive()
                val safe = VoiceResponseQueue.asciiForWatch(response)
                require(safe.isNotEmpty())
                return safe.toString(Charsets.US_ASCII).also { safe.fill(0) }
            } finally {
                val session = turn.relaySession.getAndSet(null)
                if (turn.cancelled.get()) socket.abort(session) else socket.closeTalk(session)
                turn.socket.compareAndSet(socket, null)
                socket.close()
            }
        } finally {
            credential.wipe()
        }
    }

    internal class GatewayConnectRejectedException(message: String) :
        IllegalStateException(message)

    internal class RpcSocket(
        private val credential: OpenClawVoiceCredential,
        private val store: OpenClawVoiceStore,
        private val role: String,
        private val scopes: List<String>,
        private val authToken: String,
        private val bootstrap: Boolean,
        private val events: (String, JsonObject, Long?) -> Unit = { _, _, _ -> },
    ) : WebSocketListener() {
        private val json = Json { ignoreUnknownKeys = true }
        private val pending = ConcurrentHashMap<String, CompletableFuture<JsonObject>>()
        private val connected = CompletableFuture<JsonObject>()
        private val opened = CompletableFuture<Unit>()
        private val client = OkHttpClient.Builder().pingInterval(20, TimeUnit.SECONDS).build()
        private val eventSequenceLock = Any()
        private var lastEventSequence: Long? = null
        @Volatile private var socket: WebSocket? = null

        fun connect(): JsonObject {
            socket = client.newWebSocket(Request.Builder().url(credential.url).build(), this)
            opened.get(10, TimeUnit.SECONDS)
            return connected.get(20, TimeUnit.SECONDS)
        }

        fun request(method: String, params: JsonObject, timeoutMs: Long): JsonObject {
            val id = UUID.randomUUID().toString()
            val future = CompletableFuture<JsonObject>()
            pending[id] = future
            val sent = socket?.send(buildJsonObject {
                put("type", "req"); put("id", id); put("method", method); put("params", params)
            }.toString()) == true
            if (!sent) {
                pending.remove(id)
                error("OpenClaw Gateway is disconnected")
            }
            return try { future.get(timeoutMs, TimeUnit.MILLISECONDS) }
            finally { pending.remove(id) }
        }

        override fun onOpen(webSocket: WebSocket, response: Response) {
            val expected = credential.tlsFingerprint
            if (expected != null) {
                val cert = response.handshake?.peerCertificates?.firstOrNull() as? X509Certificate
                val actual = cert?.encoded?.let { MessageDigest.getInstance("SHA-256").digest(it) }
                    ?.joinToString("") { "%02x".format(it) }
                if (actual != expected) {
                    webSocket.close(1008, "TLS identity mismatch")
                    opened.completeExceptionally(SecurityException("OpenClaw TLS identity mismatch"))
                    return
                }
            }
            opened.complete(Unit)
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            val root = runCatching { json.parseToJsonElement(text).jsonObject }.getOrNull() ?: return
            when (root["type"]?.jsonPrimitive?.content) {
                "event" -> {
                    val event = root["event"]?.jsonPrimitive?.content ?: return
                    val payload = root["payload"] as? JsonObject ?: JsonObject(emptyMap())
                    val sequence = root["seq"]?.jsonPrimitive?.content?.toLongOrNull()
                    if (!acceptEventSequence(sequence)) return
                    if (event == "connect.challenge") sendConnect(payload)
                    else events(event, payload, sequence)
                }
                "res" -> {
                    val id = root["id"]?.jsonPrimitive?.content ?: return
                    val waiter = pending[id] ?: return
                    if (root["ok"]?.jsonPrimitive?.content == "true") {
                        val payload = root["payload"] as? JsonObject ?: JsonObject(emptyMap())
                        waiter.complete(payload)
                        if (id == CONNECT_ID) connected.complete(payload)
                    } else {
                        val message = (root["error"] as? JsonObject)?.get("message")
                            ?.jsonPrimitive?.content ?: "OpenClaw request failed"
                        val failure = if (id == CONNECT_ID) {
                            GatewayConnectRejectedException(message)
                        } else {
                            IllegalStateException(message)
                        }
                        waiter.completeExceptionally(failure)
                        if (id == CONNECT_ID) connected.completeExceptionally(failure)
                    }
                }
            }
        }

        private fun acceptEventSequence(sequence: Long?): Boolean {
            if (sequence == null) return true
            synchronized(eventSequenceLock) {
                val previous = lastEventSequence
                if (previous != null && sequence <= previous) return false
                if (previous != null && sequence > previous + 1) {
                    fail(SecurityException("OpenClaw event sequence gap"))
                    socket?.cancel()
                    return false
                }
                lastEventSequence = sequence
                return true
            }
        }

        private fun sendConnect(challenge: JsonObject) {
            val nonce = challenge["nonce"]?.jsonPrimitive?.content?.trim().orEmpty()
            val issued = challenge["ts"]?.jsonPrimitive?.content?.toLongOrNull() ?: 0L
            if (nonce.isEmpty() || issued <= 0) {
                connected.completeExceptionally(IllegalStateException("Invalid OpenClaw challenge"))
                return
            }
            val canonical = listOf(
                "v3", credential.deviceId, CLIENT_ID, if (role == "node") "node" else "ui",
                role, scopes.joinToString(","), issued.toString(), authToken, nonce, "android", "android"
            ).joinToString("|")
            val device = buildJsonObject {
                put("id", credential.deviceId)
                put("publicKey", Base64.getUrlEncoder().withoutPadding().encodeToString(credential.publicKeyRaw))
                put("signature", store.sign(canonical, credential))
                put("signedAt", issued)
                put("nonce", nonce)
            }
            val params = buildJsonObject {
                put("minProtocol", 3); put("maxProtocol", 4)
                put("client", buildJsonObject {
                    put("id", CLIENT_ID); put("displayName", "Nightglass Voice")
                    put("version", "0.1.0"); put("platform", "android")
                    put("mode", if (role == "node") "node" else "ui")
                    put("deviceFamily", "Android")
                })
                put("role", role)
                if (scopes.isNotEmpty()) put("scopes", JsonArray(scopes.map(::JsonPrimitive)))
                put("auth", buildJsonObject { put(if (bootstrap) "bootstrapToken" else "token", authToken) })
                put("device", device)
                put("locale", "en-US")
            }
            val future = CompletableFuture<JsonObject>()
            pending[CONNECT_ID] = future
            future.whenComplete { value, error ->
                if (error != null) connected.completeExceptionally(error) else connected.complete(value)
            }
            socket?.send(buildJsonObject {
                put("type", "req"); put("id", CONNECT_ID); put("method", "connect"); put("params", params)
            }.toString())
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) = fail(t)
        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) =
            fail(IllegalStateException("OpenClaw Gateway closed"))

        private fun fail(error: Throwable) {
            if (!opened.isDone) opened.completeExceptionally(error)
            if (!connected.isDone) connected.completeExceptionally(error)
            pending.values.forEach { it.completeExceptionally(error) }
            pending.clear()
        }

        fun closeTalk(session: String?) {
            if (session.isNullOrBlank()) return
            runCatching {
                request("talk.session.close", buildJsonObject { put("sessionId", session) }, 8_000)
            }
        }

        fun abort(session: String?) {
            if (!session.isNullOrBlank()) {
                sendWithoutReply("talk.session.cancelOutput", buildJsonObject {
                    put("sessionId", session)
                })
                sendWithoutReply("talk.session.close", buildJsonObject {
                    put("sessionId", session)
                })
            }
            fail(CancellationException("OpenClaw voice turn cancelled"))
            socket?.cancel()
            socket = null
            client.dispatcher.executorService.shutdownNow()
            client.connectionPool.evictAll()
        }

        private fun sendWithoutReply(method: String, params: JsonObject) {
            socket?.send(buildJsonObject {
                put("type", "req")
                put("id", UUID.randomUUID().toString())
                put("method", method)
                put("params", params)
            }.toString())
        }

        fun close() {
            socket?.close(1000, "Nightglass voice turn complete")
            socket = null
            client.dispatcher.executorService.shutdown()
            client.connectionPool.evictAll()
        }

        companion object { private const val CONNECT_ID = "nightglass-connect" }
    }
}
