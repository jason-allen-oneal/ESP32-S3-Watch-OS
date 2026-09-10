package dev.nightglass.companion.premium

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.CRC32
import org.junit.Assert.*
import org.junit.Test

class PremiumContentTest {
    @Test fun reactionCaptionsAreReadableWithoutInventingUnknownActions() {
        assertEquals("React: thumbs up", PremiumContent.actionCaption("👍"))
        assertEquals("React: heart", PremiumContent.actionCaption("❤️"))
        assertEquals("Mark as read", PremiumContent.actionCaption("Mark as read"))
        assertEquals("", PremiumContent.actionCaption("🛸"))
    }
    @Test fun spokenReplyOmitsOnlyValidTerminalSuggestions() {
        for (marker in listOf("TIMER:60", "TIMER:3600", "OPEN:SPOTIFY", "OPEN:PHONE", "OPEN:INBOX", "PHONE:RING"))
            assertEquals("Review the suggestion.", PremiumContent.spokenText("Review the suggestion.\n[WATCH:$marker]"))
        for (text in listOf("OK\n[WATCH:TIMER:59]", "OK\n[WATCH:TIMER:3601]", "OK\n[WATCH:TIMER:060]",
            "OK\n[WATCH:EXEC:command]", "OK\n[WATCH:TIMER:300]\nLater", "[WATCH:OPEN:PHONE]\n[WATCH:OPEN:PHONE]"))
            assertEquals(text, PremiumContent.spokenText(text))
    }
    @Test fun documentMatchesFirmwareVector() {
        val bytes = PremiumDocument(2, "Hello", listOf(ContentAction(6, 0, "Open"))).encode()
        assertArrayEquals(byteArrayOf(1, 2, 1, 0, 5, 0, 72, 101, 108, 108, 111, 6, 0, 4, 79, 112, 101, 110), bytes)
    }
    @Test fun chunksAreBoundedContiguousAndChecksummedAtAllMtuSizes() {
        val doc = PremiumDocument(1, "Now playing", listOf(ContentAction(2, 0, "Volume -")), ByteArray(3200) { it.toByte() })
        for (mtu in listOf(20, 179, 244)) {
            val frames = PremiumContent.frames(PremiumRequest(100u, 1, 0u), doc, mtu)
            assertTrue(frames.all { it.size <= mtu })
            var offset = 0
            val payload = ArrayList<Byte>()
            frames.drop(1).dropLast(1).forEach {
                assertEquals(0x74, it[1].toInt())
                assertEquals(offset, ByteBuffer.wrap(it, 6, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() and 65535)
                payload.addAll(it.drop(8)); offset += it.size - 8
            }
            assertArrayEquals(doc.encode(), payload.toByteArray())
            assertEquals(CRC32().apply { update(doc.encode()) }.value.toInt(),
                ByteBuffer.wrap(frames.last(), 6, 4).order(ByteOrder.LITTLE_ENDIAN).int)
        }
    }
    @Test fun untrustedActionsAndOversizeArtAreRejected() {
        listOf(PremiumDocument(2, "text", listOf(ContentAction(1, 0, "Wrong kind"))),
            PremiumDocument(1, "text", listOf(ContentAction(1, 4, "Out of range"))),
            PremiumDocument(2, "text", artwork = ByteArray(3200)),
            PremiumDocument(1, "text", artwork = ByteArray(3201))).forEach {
            assertTrue(runCatching { it.encode() }.isFailure)
        }
    }
    @Test fun frameParserRejectsBadLengthsAndZeroIdentity() {
        val request = byteArrayOf(1, 0x72, 100, 0, 0, 0, 2, 42, 0, 0, 0)
        assertEquals(PremiumRequest(100u, 2, 42u), PremiumContent.request(request))
        for (size in 0 until request.size) assertNull(PremiumContent.request(request.copyOf(size)))
        request[2] = 0; assertNull(PremiumContent.request(request))
        assertNull(PremiumContent.action(ByteArray(13)))
    }
    @Test fun fencesRejectReplaysWrongTargetsAndExpiredRequests() {
        val fence = PremiumActionFence()
        val action = PremiumAction(1u, 5, 0, 42u, 1)
        fence.bind(PremiumRequest(1u, 2, 42u), listOf(ContentAction(5, 0, "Like")), 100)
        assertFalse(fence.accept(action.copy(target = 43u), 101))
        assertFalse(fence.accept(action.copy(token = 2u), 101))
        assertFalse(fence.accept(action.copy(action = 6), 101))
        assertTrue(fence.accept(action, 101))
        assertFalse(fence.accept(action, 102))
        assertTrue(fence.accept(action.copy(sequence = 2), 103))
        assertFalse(fence.accept(action.copy(sequence = 3), 60_100))
        fence.clear(); assertFalse(fence.accept(action.copy(sequence = 4), 110))
    }
    @Test fun newRequestInvalidatesPreviousActions() {
        val fence = PremiumActionFence()
        val actions = listOf(ContentAction(2, 0, "Volume -"))
        fence.bind(PremiumRequest(1u, 1, 0u), actions, 0)
        fence.bind(PremiumRequest(2u, 1, 0u), actions, 1)
        assertFalse(fence.accept(PremiumAction(1u, 2, 0, 0u, 1), 2))
        assertTrue(fence.accept(PremiumAction(2u, 2, 0, 0u, 1), 2))
    }
    @Test fun repeatedRequestDoesNotResetReplayOrExpiry() {
        val fence = PremiumActionFence()
        val owner = PremiumRequest(1u, 1, 0u)
        val actions = listOf(ContentAction(2, 0, "Volume -"))
        val action = PremiumAction(1u, 2, 0, 0u, 1)
        fence.bind(owner, actions, 0); assertTrue(fence.accept(action, 1))
        fence.bind(owner, actions, 59_000); assertFalse(fence.accept(action, 59_001))
        assertFalse(fence.accept(action.copy(sequence = 2), 60_000))
    }
}
