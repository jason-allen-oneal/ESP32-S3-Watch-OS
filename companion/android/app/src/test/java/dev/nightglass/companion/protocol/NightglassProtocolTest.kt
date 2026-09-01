package dev.nightglass.companion.protocol

import org.junit.Assert.*
import org.junit.Test

class NightglassProtocolTest {
    @Test fun watchStatusRequiresExactAuthorizedProof() {
        val authorized = NightglassProtocol.parseWatchStatus(
            byteArrayOf(1, 3, 2, 1, 1, 1))
        assertNotNull(authorized)
        assertTrue(authorized!!.authorized)
        assertFalse(NightglassProtocol.parseWatchStatus(
            byteArrayOf(1, 2, 2, 1, 1, 1))!!.authorized)
        assertFalse(NightglassProtocol.parseWatchStatus(
            byteArrayOf(1, 3, 2, 1, 1, 0))!!.authorized)
        assertNull(NightglassProtocol.parseWatchStatus(byteArrayOf(1, 3, 2, 1, 1)))
        assertNull(NightglassProtocol.parseWatchStatus(
            byteArrayOf(1, 3, 2, 1, 1, 2)))
        assertNull(NightglassProtocol.parseWatchStatus(
            byteArrayOf(1, 3, 7, 1, 1, 1)))
    }

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
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x10, 0, 2)))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x10, 9, 8)))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x11, 3, 0, 0, 0, 0)))
    }
    @Test fun mediaAndReplyResultsAreBounded() {
        val media = NightglassProtocol.mediaState("Song", "Artist", true, true)
        assertEquals(7, media[1].toInt())
        assertEquals(3, media[2].toInt())
        val result = NightglassProtocol.replyResult(7, 0, 0x78563412u, 0x11223344u)
        assertEquals(12, result.size)
        assertEquals(0x24, result[1].toInt())
    }
    @Test fun phoneFeatureFramesAreBounded() {
        val agenda = NightglassProtocol.agenda(listOf(
            NightglassProtocol.AgendaEvent(1_700_000_000L, 1_700_003_600L,
                "Standup", "Room", false)))
        assertEquals(5, agenda[1].toInt())
        assertTrue(agenda.size <= 179)
        assertArrayEquals(byteArrayOf(1, 6, 73, 3, 0),
            NightglassProtocol.phoneBattery(73, true, true))
        val callState = NightglassProtocol.callState(true, false, false,
            true, true, 0x78563412u, 2, "Incoming")
        assertEquals(8, callState[1].toInt())
        assertArrayEquals(byteArrayOf(0x12, 0x34, 0x56, 0x78, 2, 0),
            callState.copyOfRange(4, 10))
        assertThrows(IllegalArgumentException::class.java) {
            NightglassProtocol.callState(true, false, false,
                true, true, 0u, 0, "Incoming")
        }
        assertThrows(IllegalArgumentException::class.java) {
            NightglassProtocol.callState(false, false, false,
                false, false, 1u, 1, "")
        }
        val progress = NightglassProtocol.mediaState("Song", "Artist", true, true,
            true, 15_000, 120_000)
        assertEquals(7, progress[1].toInt())
        assertEquals(7, progress[2].toInt())
        assertThrows(IllegalArgumentException::class.java) {
            NightglassProtocol.agenda(List(4) {
                NightglassProtocol.AgendaEvent(1_700_000_000L, 1_700_000_001L,
                    "x", "", false)
            })
        }
    }
    @Test fun parsesBoundedPhoneActions() {
        val call = byteArrayOf(1, 0x14, 0xfe.toByte(), 0xff.toByte(), 1,
            0x12, 0x34, 0x56, 0x78, 2, 0)
        assertEquals(NightglassProtocol.WatchAction.Call(
            0xfffe, 1, 0x78563412u, 2), NightglassProtocol.parseAction(call))
        assertEquals(NightglassProtocol.WatchAction.Phone(9, 3),
            NightglassProtocol.parseAction(byteArrayOf(1, 0x15, 9, 3)))
        assertNull(NightglassProtocol.parseAction(call.copyOf().also { it[4] = 5 }))
        assertNull(NightglassProtocol.parseAction(call.copyOf().also { it[2] = 0; it[3] = 0 }))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x15, 9, 0)))
        assertNull(NightglassProtocol.parseAction(byteArrayOf(1, 0x15, 0, 3)))
    }
    @Test fun callCommandSequenceWindowRejectsReplayStaleAndAcceptsWrap() {
        assertTrue(NightglassProtocol.acceptsForwardSequence(0, 1))
        assertTrue(NightglassProtocol.acceptsForwardSequence(100, 101))
        assertFalse(NightglassProtocol.acceptsForwardSequence(100, 100))
        assertFalse(NightglassProtocol.acceptsForwardSequence(100, 99))
        assertTrue(NightglassProtocol.acceptsForwardSequence(0xffff, 1))
        assertFalse(NightglassProtocol.acceptsForwardSequence(1, 0xffff))
        assertTrue(NightglassProtocol.acceptsForwardSequence8(0xff, 1))
        assertFalse(NightglassProtocol.acceptsForwardSequence8(9, 9))
        assertFalse(NightglassProtocol.acceptsForwardSequence8(9, 8))

        val window = NightglassProtocol.CallCommandWindow()
        window.updateSession(0x11111111u, 7)
        assertTrue(window.accept(NightglassProtocol.WatchAction.Call(
            0xffff, 3, 0x11111111u, 7)))
        assertFalse(window.accept(NightglassProtocol.WatchAction.Call(
            0xffff, 3, 0x11111111u, 7))) // replay
        assertTrue(window.accept(NightglassProtocol.WatchAction.Call(
            1, 4, 0x11111111u, 7))) // wrap
        assertFalse(window.accept(NightglassProtocol.WatchAction.Call(
            2, 4, 0x22222222u, 7))) // stale/wrong call session
        assertFalse(window.accept(NightglassProtocol.WatchAction.Call(
            2, 4, 0x11111111u, 6))) // stale generation
        window.updateSession(0x22222222u, 1)
        assertTrue(window.accept(NightglassProtocol.WatchAction.Call(
            2, 3, 0x22222222u, 1)))
    }
    @Test fun provisioningContainsNoPersistentState() {
        val frame = NightglassProtocol.provisionWifi("wifi", "secret".toCharArray())
        assertEquals(0x20, frame[1].toInt())
        assertEquals(4, frame[2].toInt()); assertEquals(6, frame[3].toInt())
        assertEquals(14, NightglassProtocol.configureWeather(true, true, false, 30, 12_345_678, -87_654_321).size)
        assertArrayEquals(byteArrayOf(1, 0x22), NightglassProtocol.clearWifi())
        assertArrayEquals(byteArrayOf(1, 0x25), NightglassProtocol.forgetPeerAuthorization())
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
    @Test fun otaFramesAreMtuBoundAndStatusIsStrict() {
        val manifest = NightglassProtocol.OtaManifest(
            1, "nightglass-waveshare-esp32s3r8-2.06-v1.0", "nightglass-32m-r1",
            1, "2.0.0", 0, 4096, ByteArray(32) { 0xaa.toByte() },
            byteArrayOf(0x30, 6, 2, 1, 1, 2, 1, 1))
        val begin = NightglassProtocol.otaBegin(0x1122334478563412uL, manifest)
        assertTrue(begin.size <= 244)
        assertArrayEquals(byteArrayOf(1, 0x30, 0x12, 0x34, 0x56, 0x78, 0x44, 0x33,
            0x22, 0x11), begin.copyOfRange(0, 10))
        val data = NightglassProtocol.otaData(7uL, 230, ByteArray(230) { 0x5a })
        assertEquals(244, data.size)
        assertEquals(0x31, data[1].toInt())
        assertThrows(IllegalArgumentException::class.java) {
            NightglassProtocol.otaData(7uL, 0, ByteArray(231))
        }
        val status = byteArrayOf(1, 0x35, 7, 0, 0, 0, 0xf0.toByte(), 0xde.toByte(),
            0xbc.toByte(), 0x9a.toByte(), 2, 1, 0, 0,
            0, 0x10, 0, 0, 0xe6.toByte(), 0, 0, 0)
        assertEquals(NightglassProtocol.OtaStatus(0x9abcdef000000007uL, 2, 1, 0, 4096, 230),
            NightglassProtocol.parseOtaStatus(status))
        assertNull(NightglassProtocol.parseOtaStatus(status.copyOf().also { it[13] = 1 }))
        assertNull(NightglassProtocol.parseOtaStatus(status.copyOf().also { it[18] = 1; it[19] = 0x10 }))
        assertArrayEquals(byteArrayOf(1, 0x34, 7, 0, 0, 0, 0, 0, 0, 0),
            NightglassProtocol.otaStatusQuery(7uL))
    }
}
