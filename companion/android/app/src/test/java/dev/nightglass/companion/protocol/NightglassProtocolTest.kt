package dev.nightglass.companion.protocol

import org.junit.Assert.*
import org.junit.Test

class NightglassProtocolTest {
    @Test fun upsertMatchesFirmwareLayoutAndBounds() {
        val frame = NightglassProtocol.upsert(NightglassProtocol.RelayNotification(0x78563412u, 1, "app", "title", "x".repeat(120), true))
        assertArrayEquals(byteArrayOf(1, 1, 0x12, 0x34, 0x56, 0x78, 0xC1.toByte(), 3, 5, 96, 0), frame.copyOfRange(0, 11))
        assertEquals(11 + 3 + 5 + 96, frame.size)
        val sync = NightglassProtocol.upsert(NightglassProtocol.RelayNotification(1u, 2, "app", "title", "body"), false)
        assertEquals(2, sync[6].toInt())
    }
    @Test fun parsesMediaAndNotificationActions() {
        assertEquals(NightglassProtocol.WatchAction.Media(9, 2), NightglassProtocol.parseAction(byteArrayOf(1, 0x10, 9, 2)))
        assertEquals(NightglassProtocol.WatchAction.Notification(3, 0x78563412u, true), NightglassProtocol.parseAction(byteArrayOf(1, 0x11, 3, 0x12, 0x34, 0x56, 0x78)))
        val reply = byteArrayOf(1, 0x13, 7, 0x12, 0x34, 0x56, 0x78,
            0x44, 0x33, 0x22, 0x11, 3, 'Y'.code.toByte(), 'e'.code.toByte(), 's'.code.toByte())
        assertEquals(NightglassProtocol.WatchAction.Reply(7, 0x78563412u, 0x11223344u, "Yes"), NightglassProtocol.parseAction(reply))
        val zeroId = reply.copyOf().also { it.fill(0, 3, 7) }
        assertNull(NightglassProtocol.parseAction(zeroId))
        val invalidText = reply.copyOf().also { it[12] = 0x0a }
        assertNull(NightglassProtocol.parseAction(invalidText))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x10)))
    }
    @Test fun mediaAndReplyResultsAreBounded() {
        val media = NightglassProtocol.mediaState("Song", "Artist", true, true)
        assertEquals(4, media[1].toInt())
        assertEquals(3, media[2].toInt())
        val result = NightglassProtocol.replyResult(7, 0, 0x78563412u, 0x11223344u)
        assertEquals(12, result.size)
        assertEquals(0x24, result[1].toInt())
    }
    @Test fun provisioningContainsNoPersistentState() {
        val frame = NightglassProtocol.provisionWifi("wifi", "secret".toCharArray())
        assertEquals(0x20, frame[1].toInt())
        assertEquals(4, frame[2].toInt()); assertEquals(6, frame[3].toInt())
        assertEquals(14, NightglassProtocol.configureWeather(true, true, false, 30, 12_345_678, -87_654_321).size)
        assertArrayEquals(byteArrayOf(1, 0x22), NightglassProtocol.clearWifi())
    }
    @Test fun phoneWeatherUsesBoundedLittleEndianLayout() {
        val frame = NightglassProtocol.phoneWeather(1_700_000_000L, false, true,
            72.4, 74.1, 3, 8.7)
        assertEquals(18, frame.size)
        assertEquals(0x23, frame[1].toInt())
        assertEquals(2, frame[2].toInt())
        assertArrayEquals(byteArrayOf(0xD4.toByte(), 0x02), frame.copyOfRange(10, 12))
        assertArrayEquals(byteArrayOf(0x57, 0), frame.copyOfRange(16, 18))
    }
}
