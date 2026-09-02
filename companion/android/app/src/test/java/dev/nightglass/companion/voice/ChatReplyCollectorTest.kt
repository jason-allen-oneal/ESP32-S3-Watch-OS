package dev.nightglass.companion.voice

import java.util.concurrent.CompletableFuture
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class ChatReplyCollectorTest {
    private val sessionKey = "agent:main:dashboard:test-session"
    private fun event(json: String): JsonObject =
        Json.parseToJsonElement(json).jsonObject

    @Test fun correlatesRunAndSessionAndCompletesFromTheFinalSnapshot() {
        val completion = CompletableFuture<String>()
        val collector = ChatReplyCollector(completion)

        collector.accept(event("""{
            "runId":"other","sessionKey":"agent:main:dashboard:test-session",
            "seq":1,"state":"delta","deltaText":"wrong"
        }"""), "run-1", sessionKey)
        collector.accept(event("""{
            "runId":"run-1","sessionKey":"other","seq":1,"state":"delta",
            "deltaText":"wrong"
        }"""), "run-1", sessionKey)
        assertFalse(completion.isDone)

        collector.accept(event("""{
            "runId":"run-1","sessionKey":"agent:main:dashboard:test-session",
            "seq":1,"state":"status","phase":"starting_model"
        }"""), "run-1", sessionKey)
        collector.accept(event("""{
            "runId":"run-1","sessionKey":"agent:main:dashboard:test-session",
            "seq":2,"state":"delta","deltaText":"partial"
        }"""), "run-1", sessionKey)
        collector.accept(event("""{
            "runId":"run-1","sessionKey":"agent:main:dashboard:test-session",
            "seq":3,"state":"final","message":{"role":"assistant","content":[
              {"type":"text","text":"final "},{"type":"output_text","text":"answer"}
            ]}
        }"""), "run-1", sessionKey)

        assertEquals("final answer", completion.get(1, TimeUnit.SECONDS))
    }

    @Test fun appliesReplacementDeltasAndFallsBackToTheStream() {
        val completion = CompletableFuture<String>()
        val collector = ChatReplyCollector(completion)
        collector.accept(event("""{
            "runId":"run-2","sessionKey":"agent:main:dashboard:test-session",
            "seq":4,"state":"delta","deltaText":"discard me"
        }"""), "run-2", sessionKey)
        collector.accept(event("""{
            "runId":"run-2","sessionKey":"agent:main:dashboard:test-session",
            "seq":5,"state":"delta","deltaText":"kept","replace":true
        }"""), "run-2", sessionKey)
        collector.accept(event("""{
            "runId":"run-2","sessionKey":"agent:main:dashboard:test-session",
            "seq":6,"state":"final"
        }"""), "run-2", sessionKey)

        assertEquals("kept", completion.get(1, TimeUnit.SECONDS))
    }

    @Test fun rejectsOversizedReplies() {
        val sizeFailure = CompletableFuture<String>()
        val sizeCollector = ChatReplyCollector(sizeFailure)
        val oversized = "x".repeat(OpenClawVoiceGateway.MAX_STREAMED_REPLY_CHARS + 1)
        sizeCollector.accept(event("""{
            "runId":"run-4","sessionKey":"agent:main:dashboard:test-session",
            "seq":1,"state":"delta","deltaText":"$oversized"
        }"""), "run-4", sessionKey)
        val sizeError = assertThrows(ExecutionException::class.java) {
            sizeFailure.get(1, TimeUnit.SECONDS)
        }
        assertTrue(sizeError.cause is IllegalStateException)
    }

    @Test fun acceptsMissingGappedAndSameSequencesAndDeduplicatesExactProjections() {
        val completion = CompletableFuture<String>()
        val collector = ChatReplyCollector(completion)
        val first = event("""{
          "runId":"run-5","sessionKey":"agent:main:dashboard:test-session",
          "state":"delta","deltaText":"one"
        }""")
        collector.accept(first, "run-5", sessionKey)
        collector.accept(first, "run-5", sessionKey)
        collector.accept(event("""{
          "runId":"run-5","sessionKey":"agent:main:dashboard:test-session",
          "seq":7,"state":"delta","deltaText":" two"
        }"""), "run-5", sessionKey)
        collector.accept(event("""{
          "runId":"run-5","sessionKey":"agent:main:dashboard:test-session",
          "seq":7,"state":"final"
        }"""), "run-5", sessionKey)
        assertEquals("one two", completion.get(1, TimeUnit.SECONDS))
        assertTrue(collector.terminalReceived())
    }

    @Test fun errorAndAbortedAreTerminal() {
        listOf("error", "aborted").forEach { state ->
            val completion = CompletableFuture<String>()
            val collector = ChatReplyCollector(completion)
            collector.accept(event("""{
              "runId":"run-$state","sessionKey":"agent:main:dashboard:test-session",
              "state":"$state","errorMessage":"stopped"
            }"""), "run-$state", sessionKey)
            if (state == "aborted") {
                assertThrows(java.util.concurrent.CancellationException::class.java) {
                    completion.get(1, TimeUnit.SECONDS)
                }
            } else {
                assertThrows(ExecutionException::class.java) {
                    completion.get(1, TimeUnit.SECONDS)
                }
            }
            assertTrue(collector.terminalReceived())
        }
    }

    @Test fun permitsExactLimitAndRejectsOversizedFinalSnapshot() {
        val exact = "x".repeat(OpenClawVoiceGateway.MAX_STREAMED_REPLY_CHARS)
        val exactFuture = CompletableFuture<String>()
        ChatReplyCollector(exactFuture).accept(event("""{
          "runId":"run-exact","sessionKey":"agent:main:dashboard:test-session",
          "state":"final","message":{"role":"assistant","content":"$exact"}
        }"""), "run-exact", sessionKey)
        assertEquals(exact.length, exactFuture.get(1, TimeUnit.SECONDS).length)

        val oversized = exact + "x"
        val tooLarge = CompletableFuture<String>()
        ChatReplyCollector(tooLarge).accept(event("""{
          "runId":"run-large","sessionKey":"agent:main:dashboard:test-session",
          "state":"final","message":{"role":"assistant","content":"$oversized"}
        }"""), "run-large", sessionKey)
        assertThrows(ExecutionException::class.java) { tooLarge.get(1, TimeUnit.SECONDS) }
    }

    @Test fun blankAndNonAssistantFinalSnapshotsFailWithoutAStreamFallback() {
        listOf(
            """{"role":"assistant","content":""}""",
            """{"role":"user","content":"not an assistant reply"}""",
        ).forEachIndexed { index, message ->
            val completion = CompletableFuture<String>()
            val collector = ChatReplyCollector(completion)
            collector.accept(event("""{
              "runId":"run-empty-$index",
              "sessionKey":"agent:main:dashboard:test-session",
              "state":"delta","deltaText":"must not be accepted"
            }"""), "run-empty-$index", sessionKey)
            collector.accept(event("""{
              "runId":"run-empty-$index",
              "sessionKey":"agent:main:dashboard:test-session",
              "state":"final","message":$message
            }"""), "run-empty-$index", sessionKey)
            val failure = assertThrows(ExecutionException::class.java) {
                completion.get(1, TimeUnit.SECONDS)
            }
            assertTrue(failure.cause is IllegalStateException)
            assertTrue(collector.terminalReceived())
        }
    }
}
