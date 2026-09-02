package dev.nightglass.companion.voice

import java.io.IOException
import java.util.Base64
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okio.ByteString
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class OpenClawRpcSocketTest {
    private val json = Json { ignoreUnknownKeys = true }
    private val sessionKey = "agent:main:dashboard:6f54fe44-2c7d-48d1-a80e-053bc52feb72"

    @Test fun realWebSocketExercisesConnectSessionChatEventsAbortAndAttachmentEnvelope() {
        val server = MockWebServer()
        val requests = CopyOnWriteArrayList<JsonObject>()
        val chatEvents = CopyOnWriteArrayList<JsonObject>()
        val eventLatch = CountDownLatch(2)
        val listener = object : WebSocketListener() {
            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(code, reason)
            }

            override fun onOpen(webSocket: WebSocket, response: Response) {
                webSocket.send("""{"type":"event","event":"connect.challenge","payload":{"nonce":"fixture-nonce","ts":1700000000000}}""")
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                val request = json.parseToJsonElement(text).jsonObject
                requests += request
                val id = request.string("id")
                when (request.string("method")) {
                    "connect" -> webSocket.send(response(id, helloPayload()))
                    "sessions.create" -> webSocket.send(response(id,
                        """{"ok":true,"key":"$sessionKey"}"""))
                    "chat.send" -> {
                        webSocket.send(response(id, """{"runId":"run-wire"}"""))
                        webSocket.send(chatEvent("delta", """"deltaText":"wire """"))
                        webSocket.send(chatEvent("final",
                            """"message":{"role":"assistant","content":"wire confirmed"}"""))
                    }
                    "chat.abort" -> webSocket.send(response(id,
                        """{"ok":true,"aborted":true,"runIds":["run-wire"]}"""))
                }
            }

            private fun chatEvent(state: String, fields: String): String =
                """{"type":"event","event":"chat","payload":{"runId":"run-wire","sessionKey":"$sessionKey","state":"$state",$fields}}"""
        }
        server.enqueue(MockResponse().withWebSocketUpgrade(listener))
        server.start()
        try {
            val credential = credential(server.url("/").toString().replace("http://", "ws://"))
            val rpc = OpenClawVoiceGateway.RpcSocket(
                credential = credential, store = null, role = "operator",
                scopes = credential.scopes.sorted(), authToken = credential.operatorToken!!,
                bootstrap = false, signPayload = { canonical, _ ->
                    assertTrue(canonical.contains("fixture-nonce")); "fixture-signature"
                }, events = { event, payload, _ ->
                    if (event == "chat") { chatEvents += payload; eventLatch.countDown() }
                },
            )
            val hello = rpc.connect()
            assertEquals("operator", hello["auth"]!!.jsonObject.string("role"))
            val created = rpc.request("sessions.create",
                OpenClawVoiceGateway.sessionCreateParams(sessionKey, UUID.randomUUID().toString()), 1_000)
            assertEquals(sessionKey, created.string("key"))

            val wav = ByteArray(VoiceAudioCodec.WAV_HEADER_BYTES + 16) { it.toByte() }
            val sendParams = OpenClawVoiceGateway.chatSendParams(sessionKey, "run-wire", wav, 8)
            val ack = rpc.request("chat.send", sendParams, 1_000)
            assertEquals("run-wire", ack.string("runId"))
            assertTrue(eventLatch.await(1, TimeUnit.SECONDS))
            assertEquals(listOf("delta", "final"), chatEvents.map { it.string("state") })
            rpc.abortChat(sessionKey, "run-wire")

            val connect = requests.first { it.string("method") == "connect" }["params"]!!.jsonObject
            assertEquals("openclaw-android", connect["client"]!!.jsonObject.string("id"))
            assertEquals("fixture-signature", connect["device"]!!.jsonObject.string("signature"))
            val attachment = requests.first { it.string("method") == "chat.send" }["params"]!!
                .jsonObject["attachments"]!!.jsonArray.single().jsonObject
            assertEquals("audio/wav", attachment.string("mimeType"))
            assertEquals("nightglass-turn.wav", attachment.string("fileName"))
            assertEquals(wav.size, attachment.string("sizeBytes").toInt())
            assertEquals(1, attachment.string("durationMs").toInt())
            assertTrue(Base64.getDecoder().decode(attachment.string("content")).contentEquals(wav))
            rpc.close()
        } finally {
            server.shutdown()
        }
    }

    @Test fun realWebSocketExercisesScopeUpgradeRequestWaitAndBoundHandoff() {
        val server = MockWebServer()
        val requests = CopyOnWriteArrayList<JsonObject>()
        val listener = object : WebSocketListener() {
            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(code, reason)
            }

            override fun onOpen(webSocket: WebSocket, response: Response) {
                webSocket.send("""{"type":"event","event":"connect.challenge","payload":{"nonce":"upgrade-nonce","ts":1700000000001}}""")
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                val request = json.parseToJsonElement(text).jsonObject
                requests += request
                val id = request.string("id")
                when (request.string("method")) {
                    "connect" -> webSocket.send(response(id, """{
                      "auth":{"role":"operator","scopes":["operator.read","operator.talk"]},
                      "features":{"methods":["device.scopes.requestUpgrade","device.scopes.waitUpgrade"]}
                    }"""))
                    "device.scopes.requestUpgrade" ->
                        webSocket.send(response(id, """{"requestId":"scope-request-1"}"""))
                    "device.scopes.waitUpgrade" -> webSocket.send(response(id, """{
                      "status":"approved","requestId":"scope-request-1",
                      "deviceToken":"rotated-operator-token",
                      "scopes":["operator.read","operator.talk","operator.write"]
                    }"""))
                }
            }
        }
        server.enqueue(MockResponse().withWebSocketUpgrade(listener))
        server.start()
        try {
            val credential = credential(server.url("/").toString().replace("http://", "ws://"))
                .copy(scopes = OpenClawVoiceStore.BOOTSTRAP_SCOPES)
            val rpc = OpenClawVoiceGateway.RpcSocket(
                credential = credential, store = null, role = "operator",
                scopes = credential.scopes.sorted(), authToken = credential.operatorToken!!,
                bootstrap = false, signPayload = { _, _ -> "fixture-signature" },
            )
            rpc.connect()
            val requested = rpc.request("device.scopes.requestUpgrade", buildJsonObject {
                put("scopes", JsonArray(OpenClawVoiceStore.REQUIRED_SCOPES.sorted().map(::JsonPrimitive)))
            }, 1_000)
            val requestId = requested.string("requestId")
            val resolved = rpc.request("device.scopes.waitUpgrade", buildJsonObject {
                put("requestId", requestId)
            }, 1_000)
            val handoff = OpenClawVoiceGateway.parseApprovedScopeUpgrade(resolved, requestId)
            assertEquals("rotated-operator-token", handoff.token)
            assertEquals(OpenClawVoiceStore.REQUIRED_SCOPES, handoff.scopes)

            val upgradeParams = requests.first {
                it.string("method") == "device.scopes.requestUpgrade"
            }["params"]!!.jsonObject
            assertEquals(OpenClawVoiceStore.REQUIRED_SCOPES,
                upgradeParams["scopes"]!!.jsonArray.map { it.jsonPrimitive.content }.toSet())
            val waitParams = requests.first {
                it.string("method") == "device.scopes.waitUpgrade"
            }["params"]!!.jsonObject
            assertEquals("scope-request-1", waitParams.string("requestId"))
            rpc.close()
        } finally {
            server.shutdown()
        }
    }

    @Test fun sessionCreationUsesRestrictedAgentContractAndUniqueIdempotencyKeys() {
        val idempotencyKeys = listOf(UUID.randomUUID().toString(), UUID.randomUUID().toString())
        val paramsList = idempotencyKeys.map {
            OpenClawVoiceGateway.sessionCreateParams(sessionKey, it)
        }
        paramsList.forEach { params ->
            assertEquals("main", params.string("agentId"))
            assertEquals("Nightglass Watch", params.string("displayName"))
            assertEquals("read-only", params.string("permissionMode"))
            assertEquals(sessionKey, params.string("key"))
            UUID.fromString(params.string("idempotencyKey"))
        }
        assertNotEquals(
            paramsList[0].string("idempotencyKey"),
            paramsList[1].string("idempotencyKey"),
        )
    }

    @Test fun sessionCreationRejectsKeysOutsideTheRestrictedAgentNamespace() {
        assertThrows(IllegalArgumentException::class.java) {
            OpenClawVoiceGateway.sessionCreateParams(
                "agent:main:dashboard:wrong", UUID.randomUUID().toString())
        }
    }

    @Test fun chatSendBindsRestrictedPolicyAndServerTimeoutToTheCapturedAudio() {
        val encodedBytes = 8_000
        val wav = ByteArray(VoiceAudioCodec.WAV_HEADER_BYTES + encodedBytes * 2)
        val params = OpenClawVoiceGateway.chatSendParams(
            sessionKey = sessionKey,
            runId = "run-send",
            wav = wav,
            encodedBytes = encodedBytes,
        )
        assertEquals(sessionKey, params.string("sessionKey"))
        assertEquals("main", params.string("agentId"))
        assertEquals("read-only", params.string("expectedPermissionMode"))
        assertEquals(JsonNull, params["expectedToolOverrides"])
        assertEquals("run-send", params.string("idempotencyKey"))
        assertEquals(
            OpenClawVoiceGateway.responseTimeoutSeconds(encodedBytes) * 1_000L,
            params.string("timeoutMs").toLong(),
        )
    }

    @Test fun abortWaitsForAcknowledgementAndCarriesPerTurnCorrelation() {
        val rpc = rpcSocket()
        val sent = AtomicReference<JsonObject>()
        val requestSent = CountDownLatch(1)
        val fake = FakeWebSocket { text ->
            sent.set(json.parseToJsonElement(text).jsonObject)
            requestSent.countDown()
            true
        }
        injectSocket(rpc, fake)
        val executor = Executors.newSingleThreadExecutor()
        val abort = executor.submit { rpc.abortChat(sessionKey, "run-1") }

        assertTrue(requestSent.await(1, TimeUnit.SECONDS))
        assertFalse(abort.isDone)
        val request = sent.get()
        val params = request["params"]!!.jsonObject
        assertEquals("chat.abort", request.string("method"))
        assertEquals(sessionKey, params.string("sessionKey"))
        assertEquals("main", params.string("agentId"))
        assertEquals("run-1", params.string("runId"))
        rpc.onMessage(fake, success(
            request,
            "{\"ok\":true,\"aborted\":true,\"runIds\":[\"run-1\"]}",
            rawPayload = true,
        ))
        abort.get(1, TimeUnit.SECONDS)

        executor.shutdownNow()
        rpc.close()
    }

    @Test fun abortFailsClosedWhenTheFrameCannotBeQueuedAndSkipsUnknownRuns() {
        val rpc = rpcSocket()
        val sends = AtomicReference(0)
        val fake = FakeWebSocket {
            sends.set(sends.get() + 1)
            false
        }
        injectSocket(rpc, fake)

        assertThrows(IllegalStateException::class.java) {
            rpc.abortChat(sessionKey, "run-2")
        }
        assertEquals(1, sends.get())
        rpc.abortChat(null, "run-2")
        rpc.abortChat(sessionKey, null)
        assertEquals(1, sends.get())
        rpc.close()
    }

    @Test fun abortDoesNotTreatAnUnabortedRunAsSuccess() {
        val rpc = rpcSocket()
        val sent = AtomicReference<JsonObject>()
        val requestSent = CountDownLatch(1)
        val fake = FakeWebSocket { text ->
            sent.set(json.parseToJsonElement(text).jsonObject)
            requestSent.countDown()
            true
        }
        injectSocket(rpc, fake)
        val executor = Executors.newSingleThreadExecutor()
        val abort = executor.submit { rpc.abortChat(sessionKey, "run-still-active") }

        assertTrue(requestSent.await(1, TimeUnit.SECONDS))
        rpc.onMessage(fake, success(
            sent.get(),
            "{\"ok\":true,\"aborted\":false,\"runIds\":[]}",
            rawPayload = true,
        ))
        val failure = assertThrows(ExecutionException::class.java) {
            abort.get(1, TimeUnit.SECONDS)
        }
        assertTrue(failure.cause is IllegalArgumentException)

        executor.shutdownNow()
        rpc.close()
    }

    @Test fun transportFailureAndThreadCancellationReleasePendingWaiters() {
        val rpc = rpcSocket()
        val sent = CountDownLatch(1)
        val fake = FakeWebSocket { sent.countDown(); true }
        injectSocket(rpc, fake)
        val executor = Executors.newSingleThreadExecutor()
        val pending = executor.submit {
            rpc.request("test.wait", buildJsonObject {}, 5_000)
        }
        assertTrue(sent.await(1, TimeUnit.SECONDS))
        rpc.onFailure(fake, IOException("network lost"), null)
        val transportFailure = assertThrows(ExecutionException::class.java) {
            pending.get(1, TimeUnit.SECONDS)
        }
        assertTrue(transportFailure.cause is ExecutionException)

        val rpc2 = rpcSocket()
        val sent2 = CountDownLatch(1)
        val fake2 = FakeWebSocket { sent2.countDown(); true }
        injectSocket(rpc2, fake2)
        val worker = AtomicReference<Thread>()
        val finished = CountDownLatch(1)
        val thread = Thread {
            worker.set(Thread.currentThread())
            try {
                rpc2.request("test.cancel", buildJsonObject {}, 5_000)
            } catch (_: InterruptedException) {
                // Expected: request() must still remove its pending waiter.
            } finally {
                finished.countDown()
            }
        }
        thread.start()
        assertTrue(sent2.await(1, TimeUnit.SECONDS))
        worker.get().interrupt()
        assertTrue(finished.await(1, TimeUnit.SECONDS))
        val pendingField = OpenClawVoiceGateway.RpcSocket::class.java
            .getDeclaredField("pending").apply { isAccessible = true }
        @Suppress("UNCHECKED_CAST")
        val waiters = pendingField.get(rpc2) as Map<String, CompletableFuture<JsonObject>>
        assertTrue(waiters.isEmpty())

        executor.shutdownNow()
        rpc.close()
        rpc2.close()
    }

    @Test fun topLevelEventSequenceAllowsMissingGappedAndRepeatedValues() {
        val observed = CopyOnWriteArrayList<Long?>()
        val rpc = rpcSocket { event, _, sequence ->
            if (event == "chat") observed += sequence
        }
        val fake = FakeWebSocket { true }
        listOf<Long?>(null, 1, 8, 8).forEach { sequence ->
            val seq = sequence?.let { ",\"seq\":$it" }.orEmpty()
            rpc.onMessage(fake, """{
              "type":"event","event":"chat"$seq,
              "payload":{"runId":"run","sessionKey":"$sessionKey","state":"status"}
            }""")
        }
        assertEquals(listOf<Long?>(null, 1, 8, 8), observed)
        rpc.close()
    }

    private fun rpcSocket(
        events: (String, JsonObject, Long?) -> Unit = { _, _, _ -> },
    ): OpenClawVoiceGateway.RpcSocket {
        val credential = OpenClawVoiceCredential(
            url = "wss://gateway.invalid",
            bootstrapToken = null,
            operatorToken = "operator-token-1234",
            scopes = OpenClawVoiceStore.REQUIRED_SCOPES,
            tlsFingerprint = null,
            deviceId = "device",
            publicKeyRaw = ByteArray(32),
            privateKeyPkcs8 = ByteArray(32),
            sessionKey = null,
        )
        return OpenClawVoiceGateway.RpcSocket(
            credential = credential,
            store = null,
            role = "operator",
            scopes = credential.scopes.sorted(),
            authToken = credential.operatorToken!!,
            bootstrap = false,
            events = events,
        )
    }

    private fun credential(url: String) = OpenClawVoiceCredential(
        url = url,
        bootstrapToken = null,
        operatorToken = "operator-token-1234",
        scopes = OpenClawVoiceStore.REQUIRED_SCOPES,
        tlsFingerprint = null,
        deviceId = "device-wire",
        publicKeyRaw = ByteArray(32) { 7 },
        privateKeyPkcs8 = ByteArray(32) { 9 },
        sessionKey = null,
    )

    private fun response(id: String, payload: String): String =
        """{"type":"res","id":"$id","ok":true,"payload":$payload}"""

    private fun helloPayload(): String = """{
      "auth":{"role":"operator","scopes":["operator.read","operator.talk","operator.write"]},
      "features":{"methods":["sessions.create","chat.send","chat.abort"]}
    }"""

    private fun injectSocket(rpc: OpenClawVoiceGateway.RpcSocket, socket: WebSocket) {
        OpenClawVoiceGateway.RpcSocket::class.java.getDeclaredField("socket")
            .apply { isAccessible = true }
            .set(rpc, socket)
    }

    private fun success(
        request: JsonObject,
        payload: String,
        rawPayload: Boolean = false,
    ): String {
        val encoded = if (rawPayload) payload else """{"key":"$payload"}"""
        return """{"type":"res","id":"${request.string("id")}","ok":true,"payload":$encoded}"""
    }

    private fun JsonObject.string(key: String): String = getValue(key).jsonPrimitive.content

    private class FakeWebSocket(
        private val sendText: FakeWebSocket.(String) -> Boolean,
    ) : WebSocket {
        override fun request(): Request = Request.Builder().url("https://gateway.invalid").build()
        override fun queueSize(): Long = 0
        override fun send(text: String): Boolean = sendText(text)
        override fun send(bytes: ByteString): Boolean = false
        override fun close(code: Int, reason: String?): Boolean = true
        override fun cancel() = Unit
    }
}
