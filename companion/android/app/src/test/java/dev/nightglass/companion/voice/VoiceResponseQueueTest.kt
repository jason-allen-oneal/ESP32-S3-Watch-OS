package dev.nightglass.companion.voice

import org.junit.Assert.*
import org.junit.Test

class VoiceResponseQueueTest {
    @Test fun normalizesUnicodeToWatchAscii() {
        val encoded = VoiceResponseQueue.asciiForWatch("Caf\u00e9 \u2014 \"ready\" \ud83d\udc7b")
        assertEquals("Cafe - \"ready\" ?", encoded.toString(Charsets.US_ASCII))
        assertTrue(encoded.all { it == '\n'.code.toByte() || it.toInt() in 0x20..0x7e })
    }

    @Test fun responseIsAtomicMtuBoundAndTerminalAcked() {
        val owner = VoiceTurnOwner(7u, 4, 2)
        val queue = VoiceResponseQueue()
        assertTrue(queue.enqueueResponse(owner, 9u, "x".repeat(2048), 179))
        var frames = 0
        var terminal: VoiceTurnOwner? = null
        while (queue.size() > 0) {
            val entry = queue.beginWrite() ?: throw AssertionError("queue stalled")
            assertTrue(entry.frame.size <= 179)
            frames++
            terminal = queue.completeWrite() ?: terminal
            assertTrue(entry.frame.all { it == 0.toByte() })
        }
        assertTrue(frames > 3)
        assertEquals(owner, terminal)
        assertEquals(0, queue.size())
    }

    @Test fun purgeWipesPendingAndQueuedFrames() {
        val owner = VoiceTurnOwner(11u, 3, 8)
        val queue = VoiceResponseQueue()
        assertTrue(queue.enqueueResponse(owner, 2u, "private response", 179))
        val pending = queue.beginWrite() ?: throw AssertionError("missing pending frame")
        queue.purge(owner)
        assertTrue(pending.frame.all { it == 0.toByte() })
        assertEquals(0, queue.size())
    }

    @Test fun eventFenceRejectsStaleOwnerSessionTurnAndSequence() {
        val owner = VoiceTurnOwner(5u, 6, 7)
        val fence = VoiceEventFence(owner)
        fence.bindRelay("relay")
        assertEquals(VoiceEventFence.Decision.ACCEPT,
            fence.accept(owner, "relay", "turn", 10))
        assertEquals(VoiceEventFence.Decision.DROP,
            fence.accept(owner, "relay", "turn", 10))
        assertEquals(VoiceEventFence.Decision.DROP,
            fence.accept(owner.copy(turnGeneration = 8), "relay", "turn", 11))
        assertEquals(VoiceEventFence.Decision.DROP,
            fence.accept(owner, "other", "turn", 11))
        assertEquals(VoiceEventFence.Decision.DROP,
            fence.accept(owner, "relay", "other-turn", 11))
        fence.cancel()
        assertEquals(VoiceEventFence.Decision.DROP,
            fence.accept(owner, "relay", "turn", 12))
    }
}
