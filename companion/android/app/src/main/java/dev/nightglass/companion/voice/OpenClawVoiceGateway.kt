package dev.nightglass.companion.voice

import android.content.Context
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
import kotlinx.serialization.json.JsonNull
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
    private val resetting = AtomicBoolean(false)

    companion object {
        // Stock Gateway protocol id. A project-specific id would require an
        // OpenClaw core change and is intentionally forbidden for this client.
        internal const val CLIENT_ID = "openclaw-android"
        internal const val AGENT_ID = "main"
        internal const val SESSION_LABEL = "Nightglass Watch"
        internal const val MAX_STREAMED_REPLY_CHARS = 1_600

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

        internal fun sessionCreateParams(
            sessionKey: String,
            idempotencyKey: String,
        ): JsonObject {
            require(OpenClawVoiceStore.isNightglassSessionKey(sessionKey))
            require(idempotencyKey.isNotBlank())
            return buildJsonObject {
                put("key", sessionKey)
                put("agentId", AGENT_ID)
                put("displayName", SESSION_LABEL)
                put("permissionMode", "read-only")
                put("idempotencyKey", idempotencyKey)
            }
        }

        internal fun chatSendParams(
            sessionKey: String,
            runId: String,
            wav: ByteArray,
            encodedBytes: Int,
        ): JsonObject {
            require(OpenClawVoiceStore.isNightglassSessionKey(sessionKey))
            require(runId.isNotBlank())
            require(acceptsEncodedBytes(encodedBytes))
            require(wav.size == VoiceAudioCodec.WAV_HEADER_BYTES + encodedBytes * 2)
            return buildJsonObject {
                put("sessionKey", sessionKey)
                put("agentId", AGENT_ID)
                put("message", "Use only the Gateway-provided [Audio] Transcript for " +
                    "this Nightglass request. If it is absent, empty, or a failure " +
                    "placeholder, reply exactly: Voice transcription failed. " +
                    "Otherwise answer in plain text under 1600 characters.")
                put("deliver", false)
                put("timeoutMs", responseTimeoutSeconds(encodedBytes) * 1_000L)
                put("expectedPermissionMode", "read-only")
                put("expectedToolOverrides", JsonNull)
                put("idempotencyKey", runId)
                put("attachments", JsonArray(listOf(buildJsonObject {
                    put("type", "audio")
                    put("mimeType", "audio/wav")
                    put("fileName", "nightglass-turn.wav")
                    put("content", Base64.getEncoder().encodeToString(wav))
                    put("sizeBytes", wav.size)
                    put("durationMs", encodedBytes / 8)
                })))
            }
        }

        internal fun chatAbortParams(sessionKey: String, runId: String): JsonObject {
            require(OpenClawVoiceStore.isNightglassSessionKey(sessionKey))
            require(runId.isNotBlank())
            return buildJsonObject {
                put("sessionKey", sessionKey)
                put("agentId", AGENT_ID)
                put("runId", runId)
            }
        }

        internal fun parseOperatorHandoff(hello: JsonObject): OperatorHandoff {
            val auth = hello["auth"]?.jsonObject
                ?: error("Gateway did not return bootstrap authorization")
            val handoff = if (auth["role"]?.jsonPrimitive?.content == "operator") {
                auth
            } else {
                auth["deviceTokens"]?.jsonArray
                    ?.mapNotNull { it as? JsonObject }
                    ?.firstOrNull { it["role"]?.jsonPrimitive?.content == "operator" }
                    ?: error("Gateway did not issue a constrained operator handoff")
            }
            val scopes = handoff["scopes"]?.jsonArray
                ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
            val token = handoff["deviceToken"]?.jsonPrimitive?.content.orEmpty()
            require(OpenClawVoiceStore.scopesAreAllowedHandoff(scopes) && token.length in 16..4096)
            return OperatorHandoff(token, scopes)
        }

        internal fun parseApprovedScopeUpgrade(
            result: JsonObject,
            expectedRequestId: String,
        ): OperatorHandoff {
            require(result["status"]?.jsonPrimitive?.content == "approved")
            require(result["requestId"]?.jsonPrimitive?.content == expectedRequestId)
            val scopes = result["scopes"]?.jsonArray
                ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
            val token = result["deviceToken"]?.jsonPrimitive?.content.orEmpty()
            require(OpenClawVoiceStore.scopesAreExactlyRequired(scopes) && token.length in 16..4096)
            return OperatorHandoff(token, scopes)
        }
    }

    private class ActiveTurn(val owner: VoiceTurnOwner) {
        val cancelled = AtomicBoolean(false)
        val socket = AtomicReference<RpcSocket?>(null)
        val runId = AtomicReference<String?>(null)
        val sessionKey = AtomicReference<String?>(null)
        val runMayExist = AtomicBoolean(false)
        val assistant = AtomicReference<CompletableFuture<String>?>(null)
        @Volatile var worker: FutureTask<Unit>? = null

        fun cancel() {
            if (!cancelled.compareAndSet(false, true)) return
            assistant.get()?.completeExceptionally(
                CancellationException("OpenClaw voice turn cancelled"))
            socket.get()?.cancelPendingWaits()
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
            if (active != null || resetting.get()) {
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

    /** Create a fresh restricted-agent session and replace the active pointer. */
    fun newConversation(completed: (Result<Unit>) -> Unit): Boolean {
        if (!resetting.compareAndSet(false, true)) return false
        synchronized(activeLock) {
            if (active != null) {
                resetting.set(false)
                return false
            }
        }
        executor.execute {
            val result = runCatching { createNewDedicatedSession() }
            resetting.set(false)
            completed(result)
        }
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
        executor.shutdown()
    }

    private fun runTurn(turn: ActiveTurn, mulaw: ByteArray): String {
        var credential = store.load() ?: error("OpenClaw voice is not configured")
        try {
            turn.checkActive()
            if (credential.operatorToken == null) {
                status("Pairing constrained OpenClaw voice identity")
                val bootstrap = credential.bootstrapToken ?: error("OpenClaw setup code expired or was consumed")
                val socket = RpcSocket(
                    credential,
                    store,
                    "operator",
                    OpenClawVoiceStore.BOOTSTRAP_SCOPES.sorted(),
                    bootstrap,
                    true,
                )
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
            if (!OpenClawVoiceStore.scopesAreExactlyRequired(credential.scopes)) {
                val upgraded = upgradeOperatorCredential(turn, credential)
                credential.wipe()
                credential = upgraded
            }

            status("Connecting to OpenClaw")
            val assistant = CompletableFuture<String>()
            turn.assistant.set(assistant)
            val collector = ChatReplyCollector(assistant)
            val socket = RpcSocket(
                credential, store, "operator", OpenClawVoiceStore.REQUIRED_SCOPES.sorted(),
                credential.operatorToken ?: error("OpenClaw voice authorization missing"), false,
            ) { event, payload, sequence ->
                if (event == "chat") collector.accept(
                    payload,
                    turn.runId.get(),
                    turn.sessionKey.get().orEmpty(),
                )
            }
            turn.socket.set(socket)
            try {
                val hello = socket.connect()
                turn.checkActive()
                verifyOperatorHello(hello)

                val sessionKey = ensureDedicatedSession(socket, credential)
                turn.sessionKey.set(sessionKey)
                turn.checkActive()
                status("Sending voice to OpenClaw")
                val wav = VoiceAudioCodec.mulaw8kToWav(mulaw)
                try {
                    turn.checkActive()
                    val idempotencyKey = UUID.randomUUID().toString()
                    turn.runId.set(idempotencyKey)
                    // The Gateway may accept the frame before the ACK reaches
                    // the phone. From this point onward, every non-terminal
                    // exit must issue an acknowledged, idempotent abort.
                    turn.runMayExist.set(true)
                    val sent = socket.request("chat.send",
                        chatSendParams(sessionKey, idempotencyKey, wav, mulaw.size), 30_000)
                    val returnedRunId = sent["runId"]?.jsonPrimitive?.content
                        ?: error("OpenClaw did not acknowledge the chat run")
                    require(returnedRunId == idempotencyKey) {
                        "OpenClaw acknowledged an unexpected chat run"
                    }
                } finally { wav.fill(0) }
                turn.checkActive()
                status("OpenClaw is thinking")
                val response = assistant.get(responseTimeoutSeconds(mulaw.size), TimeUnit.SECONDS)
                turn.checkActive()
                val safe = VoiceResponseQueue.asciiForWatch(response)
                require(safe.isNotEmpty())
                return safe.toString(Charsets.US_ASCII).also { safe.fill(0) }
            } finally {
                val runId = turn.runId.getAndSet(null)
                val sessionKey = turn.sessionKey.getAndSet(null)
                try {
                    if (turn.runMayExist.get() && !collector.terminalReceived()) {
                        abortAcceptedRun(credential, socket, sessionKey, runId)
                    }
                } finally {
                    turn.assistant.set(null)
                    turn.socket.compareAndSet(socket, null)
                    socket.close()
                }
            }
        } finally {
            credential.wipe()
        }
    }

    private fun upgradeOperatorCredential(
        turn: ActiveTurn,
        credential: OpenClawVoiceCredential,
    ): OpenClawVoiceCredential {
        require(credential.operatorToken != null &&
            credential.scopes == OpenClawVoiceStore.BOOTSTRAP_SCOPES) {
            "OpenClaw voice authorization has an unexpected scope profile"
        }
        status("Requesting Nightglass operator.write approval")
        val socket = RpcSocket(
            credential,
            store,
            "operator",
            credential.scopes.sorted(),
            credential.operatorToken ?: error("OpenClaw voice authorization missing"),
            false,
        )
        turn.socket.set(socket)
        try {
            val hello = socket.connect()
            turn.checkActive()
            verifyBootstrapOperatorHello(hello)
            val registration = socket.request("device.scopes.requestUpgrade", buildJsonObject {
                put("scopes", JsonArray(OpenClawVoiceStore.REQUIRED_SCOPES.sorted()
                    .map(::JsonPrimitive)))
            }, 30_000)
            val requestId = registration["requestId"]?.jsonPrimitive?.content
                ?: error("OpenClaw did not register the scope upgrade")
            status("Approve the Nightglass scope upgrade in OpenClaw")
            val resolved = socket.request("device.scopes.waitUpgrade", buildJsonObject {
                put("requestId", requestId)
            }, 310_000)
            turn.checkActive()
            when (resolved["status"]?.jsonPrimitive?.content) {
                "approved" -> Unit
                "rejected" -> error("Nightglass scope upgrade was rejected")
                "expired" -> error("Nightglass scope upgrade expired")
                else -> error("OpenClaw returned an invalid scope-upgrade result")
            }
            val handoff = parseApprovedScopeUpgrade(resolved, requestId)
            check(store.persistOperatorToken(credential, handoff.token, handoff.scopes)) {
                "OpenClaw setup changed during scope approval; try again"
            }
            return store.load() ?: error("OpenClaw scope upgrade was not persisted")
        } finally {
            turn.socket.compareAndSet(socket, null)
            socket.close()
        }
    }

    private fun createNewDedicatedSession() {
        var credential = store.load() ?: error("OpenClaw voice is not configured")
        try {
            require(credential.operatorToken != null &&
                OpenClawVoiceStore.scopesAreExactlyRequired(credential.scopes)) {
                "OpenClaw voice requires the approved read+talk+write scope profile"
            }
            val socket = RpcSocket(
                credential, store, "operator", OpenClawVoiceStore.REQUIRED_SCOPES.sorted(),
                credential.operatorToken ?: error("OpenClaw voice authorization missing"), false,
            )
            try {
                val hello = socket.connect()
                verifyOperatorHello(hello)
                val sessionKey = createDedicatedSession(socket)
                check(store.replaceSessionKey(credential, credential.sessionKey, sessionKey)) {
                    "OpenClaw setup changed while starting a new conversation; try again"
                }
                status("Started a new Nightglass watch conversation")
            } finally { socket.close() }
        } finally { credential.wipe() }
    }

    private fun ensureDedicatedSession(
        socket: RpcSocket,
        credential: OpenClawVoiceCredential,
    ): String {
        credential.sessionKey?.let {
            require(OpenClawVoiceStore.isNightglassSessionKey(it))
            return it
        }
        val sessionKey = createDedicatedSession(socket)
        check(store.replaceSessionKey(credential, null, sessionKey)) {
            "OpenClaw setup changed while creating the watch session; try again"
        }
        return sessionKey
    }

    private fun createDedicatedSession(socket: RpcSocket): String {
        val requestedKey = OpenClawVoiceStore.SESSION_KEY_PREFIX + UUID.randomUUID()
        val created = socket.request("sessions.create",
            sessionCreateParams(requestedKey, UUID.randomUUID().toString()), 30_000)
        require(created["ok"]?.jsonPrimitive?.content == "true") {
            "OpenClaw did not confirm watch session creation"
        }
        val key = created["key"]?.jsonPrimitive?.content.orEmpty()
        require(key == requestedKey && OpenClawVoiceStore.isNightglassSessionKey(key)) {
            "OpenClaw created an unexpected watch session"
        }
        return key
    }

    /**
     * Require a Gateway acknowledgement for every run that might have been
     * admitted. If the turn socket died after submission, retry once over a
     * fresh authenticated connection using the same paired device identity.
     * chat.send also carries a server-side timeout, so a total network outage
     * cannot leave an unbounded run behind.
     */
    private fun abortAcceptedRun(
        credential: OpenClawVoiceCredential,
        turnSocket: RpcSocket,
        sessionKey: String?,
        runId: String?,
    ) {
        if (sessionKey.isNullOrBlank() || runId.isNullOrBlank()) return
        val firstFailure = runCatching { turnSocket.abortChat(sessionKey, runId) }.exceptionOrNull()
            ?: return
        val cleanupSocket = RpcSocket(
            credential, store, "operator", OpenClawVoiceStore.REQUIRED_SCOPES.sorted(),
            credential.operatorToken ?: error("OpenClaw voice authorization missing"), false,
        )
        try {
            verifyOperatorHello(cleanupSocket.connect())
            cleanupSocket.abortChat(sessionKey, runId)
        } catch (retryFailure: Throwable) {
            retryFailure.addSuppressed(firstFailure)
            throw retryFailure
        } finally {
            cleanupSocket.close()
        }
    }

    private fun verifyOperatorHello(hello: JsonObject) {
        require(hello["auth"]?.jsonObject?.get("role")?.jsonPrimitive?.content == "operator")
        val scopes = hello["auth"]?.jsonObject?.get("scopes")?.jsonArray
            ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
        require(OpenClawVoiceStore.scopesAreExactlyRequired(scopes)) {
            "OpenClaw voice requires the approved read+talk+write scope profile"
        }
        val methods = hello["features"]?.jsonObject?.get("methods")?.jsonArray
            ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
        require("sessions.create" in methods && "chat.send" in methods &&
            "chat.abort" in methods)
    }

    private fun verifyBootstrapOperatorHello(hello: JsonObject) {
        require(hello["auth"]?.jsonObject?.get("role")?.jsonPrimitive?.content == "operator")
        val scopes = hello["auth"]?.jsonObject?.get("scopes")?.jsonArray
            ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
        require(scopes == OpenClawVoiceStore.BOOTSTRAP_SCOPES) {
            "OpenClaw voice bootstrap has an unexpected scope profile"
        }
        val methods = hello["features"]?.jsonObject?.get("methods")?.jsonArray
            ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
        require("device.scopes.requestUpgrade" in methods &&
            "device.scopes.waitUpgrade" in methods)
    }

    internal class GatewayConnectRejectedException(message: String) :
        IllegalStateException(message)

    internal class RpcSocket(
        private val credential: OpenClawVoiceCredential,
        private val store: OpenClawVoiceStore? = null,
        private val role: String,
        private val scopes: List<String>,
        private val authToken: String,
        private val bootstrap: Boolean,
        private val client: OkHttpClient =
            OkHttpClient.Builder().pingInterval(20, TimeUnit.SECONDS).build(),
        private val signPayload: (String, OpenClawVoiceCredential) -> String =
            { payload, value -> requireNotNull(store).sign(payload, value) },
        private val events: (String, JsonObject, Long?) -> Unit = { _, _, _ -> },
    ) : WebSocketListener() {
        private val json = Json { ignoreUnknownKeys = true }
        private val pending = ConcurrentHashMap<String, CompletableFuture<JsonObject>>()
        private val connected = CompletableFuture<JsonObject>()
        private val opened = CompletableFuture<Unit>()
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
                put("signature", signPayload(canonical, credential))
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

        fun cancelPendingWaits() {
            val error = CancellationException("OpenClaw voice turn cancelled")
            if (!opened.isDone) opened.completeExceptionally(error)
            if (!connected.isDone) connected.completeExceptionally(error)
            pending.values.forEach { it.completeExceptionally(error) }
            pending.clear()
        }

        fun abortChat(sessionKey: String?, runId: String?) {
            if (!sessionKey.isNullOrBlank() && !runId.isNullOrBlank()) {
                val result = request("chat.abort", chatAbortParams(sessionKey, runId), 10_000)
                require(result["ok"]?.jsonPrimitive?.content == "true") {
                    "OpenClaw did not confirm chat cancellation"
                }
                val aborted = result["aborted"]?.jsonPrimitive?.content?.toBooleanStrictOrNull()
                    ?: error("OpenClaw returned an invalid cancellation acknowledgement")
                require(aborted) {
                    "OpenClaw did not confirm that the accepted run was cancelled"
                }
                val runIds = result["runIds"]?.jsonArray
                    ?.map { it.jsonPrimitive.content }?.toSet().orEmpty()
                require(runId in runIds) {
                    "OpenClaw acknowledged cancellation for an unexpected run"
                }
            }
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

/** Bounded, run-correlated projection of stock Gateway chat events. */
internal class ChatReplyCollector(
    private val completion: CompletableFuture<String>,
) {
    private val streamed = StringBuilder()
    private val seenProjections = mutableSetOf<String>()
    private var terminal = false

    @Synchronized
    fun terminalReceived(): Boolean = terminal

    @Synchronized
    fun accept(payload: JsonObject, expectedRunId: String?, expectedSessionKey: String) {
        if (completion.isDone || expectedRunId.isNullOrBlank()) return
        if (payload["runId"]?.asString() != expectedRunId ||
            payload["sessionKey"]?.asString() != expectedSessionKey) return
        if (!seenProjections.add(payload.toString())) return
        if (seenProjections.size > MAX_EVENT_PROJECTIONS) {
            return fail(IllegalStateException("OpenClaw sent too many chat events"))
        }

        when (payload["state"]?.asString()) {
            "status" -> Unit
            "delta" -> {
                val delta = payload["deltaText"]?.asString().orEmpty()
                if (payload["replace"]?.asString() == "true") {
                    streamed.setLength(0)
                }
                if (streamed.length + delta.length >
                    OpenClawVoiceGateway.MAX_STREAMED_REPLY_CHARS) {
                    return fail(IllegalStateException("OpenClaw reply exceeded the watch limit"))
                }
                streamed.append(delta)
            }
            "final" -> {
                terminal = true
                val finalText = if ("message" in payload) {
                    extractAssistantText(payload["message"])
                        ?: return fail(IllegalStateException(
                            "OpenClaw returned an invalid final assistant response"))
                } else {
                    streamed.toString()
                }
                if (finalText.isBlank()) {
                    fail(IllegalStateException("OpenClaw returned an empty response"))
                } else if (finalText.length > OpenClawVoiceGateway.MAX_STREAMED_REPLY_CHARS) {
                    fail(IllegalStateException("OpenClaw reply exceeded the watch limit"))
                } else {
                    completion.complete(finalText)
                }
            }
            "error" -> {
                terminal = true
                fail(IllegalStateException(
                    payload["errorMessage"]?.asString() ?: "OpenClaw chat failed"))
            }
            "aborted" -> {
                terminal = true
                fail(CancellationException(
                    payload["errorMessage"]?.asString() ?: "OpenClaw chat was cancelled"))
            }
        }
    }

    private fun fail(error: Throwable) {
        completion.completeExceptionally(error)
    }

    companion object {
        private const val MAX_EVENT_PROJECTIONS = 512

        internal fun extractAssistantText(element: JsonElement?): String? {
            val message = element as? JsonObject ?: return null
            if (message["role"]?.asString() != "assistant") return null
            return when (val content = message["content"]) {
                is JsonPrimitive -> content.content.takeIf { it.isNotBlank() }
                is JsonArray -> content.mapNotNull { part ->
                    val block = part as? JsonObject ?: return@mapNotNull null
                    if (block["type"]?.asString() !in setOf("text", "output_text")) {
                        return@mapNotNull null
                    }
                    block["text"]?.asString()?.takeIf { it.isNotBlank() }
                }.joinToString("").takeIf { it.isNotBlank() }
                else -> null
            }
        }
    }
}

private fun JsonElement.asString(): String? = (this as? JsonPrimitive)?.content
