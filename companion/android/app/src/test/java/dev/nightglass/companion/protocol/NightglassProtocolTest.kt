package dev.nightglass.companion.protocol

import org.junit.Assert.*
import org.junit.Test

class NightglassProtocolTest {
    @Test fun upsertMatchesFirmwareLayoutAndBounds() {
        val frame = NightglassProtocol.upsert(NightglassProtocol.RelayNotification(0x78563412u, 1, "app", "title", "x".repeat(120)))
        assertArrayEquals(byteArrayOf(1, 1, 0x12, 0x34, 0x56, 0x78, 1, 3, 5, 96, 0), frame.copyOfRange(0, 11))
        assertEquals(11 + 3 + 5 + 96, frame.size)
    }
    @Test fun parsesMediaAndNotificationActions() {
        assertEquals(NightglassProtocol.WatchAction.Media(9, 2), NightglassProtocol.parseAction(byteArrayOf(1, 0x10, 9, 2)))
        assertEquals(NightglassProtocol.WatchAction.Notification(3, 0x78563412u, true), NightglassProtocol.parseAction(byteArrayOf(1, 0x11, 3, 0x12, 0x34, 0x56, 0x78)))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x10)))
    }
    @Test fun provisioningContainsNoPersistentState() {
        val frame = NightglassProtocol.provisionWifi("wifi", "secret".toCharArray())
        assertEquals(0x20, frame[1].toInt())
        assertEquals(4, frame[2].toInt()); assertEquals(6, frame[3].toInt())
        assertEquals(14, NightglassProtocol.configureWeather(true, true, false, 30, 12_345_678, -87_654_321).size)
        assertArrayEquals(byteArrayOf(1, 0x22), NightglassProtocol.clearWifi())
    }
}
