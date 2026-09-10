package dev.nightglass.companion.voice

import dev.nightglass.companion.protocol.NightglassProtocol
import org.junit.Assert.*
import org.junit.Test

class VoiceTransferReceiverTest {
    @Test fun boundedTransferAcksAndCompletesWithoutRetention() {
        val writes = mutableListOf<ByteArray>()
        var completed: ByteArray? = null
        val receiver = VoiceTransferReceiver(writes::add, complete = { _, audio -> completed = audio })
        val audio = "Nightglas".toByteArray()
        val crc = VoiceTransferReceiver.crc32(audio)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(7u, audio.size, crc)))
        assertTrue(receiver.active())
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(7u, 1, 0,
            audio.copyOfRange(0, 4))))
        assertFalse(receiver.accept(NightglassProtocol.VoiceRequest.Data(7u, 3, 6,
            audio.copyOfRange(6, 9))))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(7u, 2, 4,
            audio.copyOfRange(4, 9))))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.End(7u, audio.size, crc)))
        assertArrayEquals(audio, completed)
        assertFalse(receiver.active())
        assertTrue(writes.size >= 3)
    }

    @Test fun crcFailureAndDisconnectWipeState() {
        val receiver = VoiceTransferReceiver({}, complete = { _, _ -> fail("must not complete") })
        val audio = byteArrayOf(1, 2, 3)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(9u, 3, 1u)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(9u, 1, 0, audio)))
        assertFalse(receiver.accept(NightglassProtocol.VoiceRequest.End(9u, 3, 1u)))
        assertFalse(receiver.active())
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(
            10u, 3, VoiceTransferReceiver.crc32(audio))))
        receiver.linkLost()
        assertFalse(receiver.active())
    }

    @Test fun spokenOptInIsForwardedOnlyAfterVerifiedUpload() {
        var spoken = false
        var completed = false
        val audio = byteArrayOf(4, 8, 15, 16)
        val receiver = VoiceTransferReceiver(
            {},
            complete = { _, received ->
                completed = true
                received.fill(0)
            },
            spokenReplies = { _, enabled -> spoken = enabled },
        )
        val crc = VoiceTransferReceiver.crc32(audio)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(
            17u, audio.size, crc, spokenReplies = true)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(
            17u, 1, 0, audio)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.End(17u, audio.size, crc)))
        assertTrue(completed)
        assertTrue(spoken)
    }

    @Test fun discordDestinationIsForwardedOnlyAfterVerifiedUpload() {
        var discord = false
        var completed = false
        val audio = byteArrayOf(3, 1, 4, 1, 5)
        val receiver = VoiceTransferReceiver(
            {},
            complete = { _, received ->
                completed = true
                received.fill(0)
            },
            discordReply = { _, enabled -> discord = enabled },
        )
        val crc = VoiceTransferReceiver.crc32(audio)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(
            18u, audio.size, crc, discordReply = true)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(
            18u, 1, 0, audio)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.End(18u, audio.size, crc)))
        assertTrue(completed)
        assertTrue(discord)

        var failedDestination = false
        val failed = VoiceTransferReceiver(
            {},
            complete = { _, _ -> fail("must not complete") },
            discordReply = { _, enabled -> failedDestination = enabled },
        )
        assertTrue(failed.accept(NightglassProtocol.VoiceRequest.Begin(
            19u, audio.size, crc + 1u, discordReply = true)))
        assertTrue(failed.accept(NightglassProtocol.VoiceRequest.Data(
            19u, 1, 0, audio)))
        assertFalse(failed.accept(NightglassProtocol.VoiceRequest.End(
            19u, audio.size, crc + 1u)))
        assertFalse(failedDestination)
    }

    @Test fun discordBeginRejectsTheGeneralFiveMinuteBound() {
        val writes = mutableListOf<ByteArray>()
        val receiver = VoiceTransferReceiver(writes::add, complete = { _, _ ->
            fail("oversized Discord turn must not complete")
        })
        assertFalse(receiver.accept(NightglassProtocol.VoiceRequest.Begin(
            20u, NightglassProtocol.MAX_DISCORD_VOICE_REPLY_BYTES + 1,
            1u, discordReply = true)))
        assertFalse(receiver.active())
        assertEquals(3, writes.single()[11].toInt())
    }

    @Test fun codecBuildsStandardEightKhzMonoPcm16Wav() {
        val wav = VoiceAudioCodec.mulaw8kToWav(
            byteArrayOf(0xff.toByte(), 0x80.toByte()))
        assertEquals(48, wav.size)
        assertEquals("RIFF", wav.copyOfRange(0, 4).toString(Charsets.US_ASCII))
        assertEquals(40, le32(wav, 4))
        assertEquals("WAVE", wav.copyOfRange(8, 12).toString(Charsets.US_ASCII))
        assertEquals("fmt ", wav.copyOfRange(12, 16).toString(Charsets.US_ASCII))
        assertEquals(16, le32(wav, 16))
        assertEquals(1, le16(wav, 20))
        assertEquals(1, le16(wav, 22))
        assertEquals(8_000, le32(wav, 24))
        assertEquals(16_000, le32(wav, 28))
        assertEquals(2, le16(wav, 32))
        assertEquals(16, le16(wav, 34))
        assertEquals("data", wav.copyOfRange(36, 40).toString(Charsets.US_ASCII))
        assertEquals(4, le32(wav, 40))
        assertArrayEquals(byteArrayOf(0, 0, 0x7c, 0x7d), wav.copyOfRange(44, 48))
    }

    @Test fun codecEnforcesTheFiveMinuteAttachmentBound() {
        val maximum = VoiceAudioCodec.MAX_MULAW_WAV_BYTES
        assertEquals(NightglassProtocol.MAX_VOICE_ENCODED_BYTES, maximum)
        val encoded = ByteArray(maximum) { 0xff.toByte() }
        val wav = VoiceAudioCodec.mulaw8kToWav(encoded)
        assertEquals(VoiceAudioCodec.WAV_HEADER_BYTES + maximum * 2, wav.size)
        assertEquals(maximum * 2, le32(wav, 40))
        assertTrue(wav.copyOfRange(44, 76).all { it == 0.toByte() })
        assertThrows(IllegalArgumentException::class.java) {
            VoiceAudioCodec.mulaw8kToWav(ByteArray(0))
        }
        assertThrows(IllegalArgumentException::class.java) {
            VoiceAudioCodec.mulaw8kToWav(ByteArray(maximum + 1))
        }
        wav.fill(0)
        encoded.fill(0)
    }

    @Test fun gatewayAcceptsFiveMinuteBoundAndScalesResponseTimeout() {
        val maximum = NightglassProtocol.MAX_VOICE_ENCODED_BYTES
        assertTrue(OpenClawVoiceGateway.acceptsEncodedBytes(1))
        assertTrue(OpenClawVoiceGateway.acceptsEncodedBytes(maximum))
        assertFalse(OpenClawVoiceGateway.acceptsEncodedBytes(0))
        assertFalse(OpenClawVoiceGateway.acceptsEncodedBytes(maximum + 1))
        assertEquals(61L, OpenClawVoiceGateway.responseTimeoutSeconds(8_000))
        assertEquals(360L, OpenClawVoiceGateway.responseTimeoutSeconds(maximum))
    }

    @Test fun acceptsMaximumFiveMinuteTransferAndFencesStaleCancel() {
        val maximum = NightglassProtocol.MAX_VOICE_ENCODED_BYTES
        val source = ByteArray(maximum) { index -> (index * 31).toByte() }
        val crc = VoiceTransferReceiver.crc32(source)
        var completed: ByteArray? = null
        val cancelled = mutableListOf<UInt>()
        val receiver = VoiceTransferReceiver({}, { _, audio -> completed = audio }, cancelled::add)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(41u, maximum, crc)))
        assertFalse(receiver.accept(NightglassProtocol.VoiceRequest.Cancel(42u, 1)))
        assertTrue(receiver.active())
        var offset = 0
        var sequence = 1
        while (offset < source.size) {
            val end = minOf(offset + 232, source.size)
            assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(
                41u, sequence, offset, source.copyOfRange(offset, end))))
            offset = end
            sequence++
        }
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.End(41u, maximum, crc)))
        assertFalse(receiver.active())
        assertArrayEquals(source, completed)
        assertEquals(listOf(42u), cancelled)
        completed?.fill(0)
        source.fill(0)
    }

    @Test fun cancelRemainsObservableAfterUploadCompletion() {
        val cancelled = mutableListOf<UInt>()
        val audio = byteArrayOf(1, 2, 3)
        val receiver = VoiceTransferReceiver({}, { _, completed -> completed.fill(0) },
            cancelled::add)
        val crc = VoiceTransferReceiver.crc32(audio)
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Begin(12u, audio.size, crc)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.Data(12u, 1, 0, audio)))
        assertTrue(receiver.accept(NightglassProtocol.VoiceRequest.End(12u, audio.size, crc)))
        assertFalse(receiver.accept(NightglassProtocol.VoiceRequest.Cancel(12u, 1)))
        assertEquals(listOf(12u), cancelled)
    }

    private fun le16(bytes: ByteArray, offset: Int): Int =
        (bytes[offset].toInt() and 0xff) or
            ((bytes[offset + 1].toInt() and 0xff) shl 8)

    private fun le32(bytes: ByteArray, offset: Int): Int =
        le16(bytes, offset) or (le16(bytes, offset + 2) shl 16)
}
