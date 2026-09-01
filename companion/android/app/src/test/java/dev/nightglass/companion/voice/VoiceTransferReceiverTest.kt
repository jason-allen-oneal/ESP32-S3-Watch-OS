package dev.nightglass.companion.voice

import dev.nightglass.companion.protocol.NightglassProtocol
import java.util.Base64
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

    @Test fun codecExpandsEightKhzToTwentyFourKhz() {
        val pcm = VoiceAudioCodec.mulaw8kToPcm24k(byteArrayOf(0xff.toByte(), 0x80.toByte()))
        assertEquals(12, pcm.size)
        assertArrayEquals(ByteArray(6), pcm.copyOfRange(0, 6))
        assertArrayEquals(byteArrayOf(0x7c, 0x7d, 0x7c, 0x7d, 0x7c, 0x7d),
            pcm.copyOfRange(6, 12))
    }

    @Test fun codecDecodesBoundedChunksWithoutWholeTurnPcm() {
        val maximum = VoiceAudioCodec.MAX_MULAW_CHUNK_BYTES
        val encoded = ByteArray(maximum + 2) { 0xff.toByte() }
        val pcm = VoiceAudioCodec.mulaw8kChunkToPcm24k(encoded, 1, maximum)
        assertEquals(VoiceAudioCodec.MAX_PCM_CHUNK_BYTES, pcm.size)
        assertTrue(pcm.all { it == 0.toByte() })
        assertTrue(Base64.getEncoder().encodeToString(pcm).length < 512 * 1024)
        assertThrows(IllegalArgumentException::class.java) {
            VoiceAudioCodec.mulaw8kChunkToPcm24k(encoded, 0, maximum + 1)
        }
        pcm.fill(0)
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
}
