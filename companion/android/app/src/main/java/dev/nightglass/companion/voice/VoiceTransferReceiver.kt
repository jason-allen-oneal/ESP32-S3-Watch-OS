package dev.nightglass.companion.voice

import dev.nightglass.companion.protocol.NightglassProtocol
import java.util.zip.CRC32

class VoiceTransferReceiver(
    private val write: (ByteArray) -> Unit,
    private val complete: (UInt, ByteArray) -> Unit,
    private val cancelled: (UInt) -> Unit = {},
) {
    private var session = 0u
    private var expectedBytes = 0
    private var expectedCrc = 0u
    private var received = 0
    private var nextSequence = 1
    private var chunksSinceAck = 0
    private var buffer: ByteArray? = null

    @Synchronized fun accept(request: NightglassProtocol.VoiceRequest): Boolean {
        return when (request) {
            is NightglassProtocol.VoiceRequest.Begin -> begin(request)
            is NightglassProtocol.VoiceRequest.Data -> data(request)
            is NightglassProtocol.VoiceRequest.End -> end(request)
            is NightglassProtocol.VoiceRequest.Cancel -> cancel(request)
        }
    }

    @Synchronized fun linkLost() = reset()

    @Synchronized fun active(): Boolean = buffer != null

    private fun begin(request: NightglassProtocol.VoiceRequest.Begin): Boolean {
        if (buffer != null) {
            write(NightglassProtocol.voiceAck(request.sessionId, 0, 0, 2))
            return false
        }
        session = request.sessionId
        expectedBytes = request.totalBytes
        expectedCrc = request.crc32
        received = 0
        nextSequence = 1
        chunksSinceAck = 0
        buffer = try {
            ByteArray(expectedBytes)
        } catch (_: OutOfMemoryError) {
            write(NightglassProtocol.voiceAck(session, 0, 0, 8))
            reset()
            return false
        }
        write(NightglassProtocol.voiceAck(session, 0, 4, 0))
        return true
    }

    private fun data(request: NightglassProtocol.VoiceRequest.Data): Boolean {
        val target = buffer ?: return false
        if (request.sessionId != session) return false
        if (request.offset < received) {
            write(NightglassProtocol.voiceAck(session, received, 4, 0))
            return true
        }
        if (request.offset != received || request.sequence != nextSequence ||
            request.payload.size > target.size - received) {
            write(NightglassProtocol.voiceAck(session, received, 4, 0))
            return false
        }
        request.payload.copyInto(target, received)
        received += request.payload.size
        nextSequence = if (nextSequence == 0xffff) 1 else nextSequence + 1
        chunksSinceAck++
        if (chunksSinceAck == 4 || received == expectedBytes) {
            chunksSinceAck = 0
            write(NightglassProtocol.voiceAck(session, received, 4, 0))
        }
        return true
    }

    private fun end(request: NightglassProtocol.VoiceRequest.End): Boolean {
        val target = buffer ?: return false
        if (request.sessionId != session || request.totalBytes != expectedBytes ||
            request.crc32 != expectedCrc || received != expectedBytes ||
            crc32(target) != expectedCrc) {
            write(NightglassProtocol.voiceAck(request.sessionId, received, 0, 3))
            reset()
            return false
        }
        val completedSession = session
        // Transfer the receiver's only reference instead of making a second
        // multi-megabyte copy. The completion owner must wipe the array after
        // submission; linkLost/reset can no longer reach it after this point.
        buffer = null
        val completedBytes = received
        clearState()
        try {
            write(NightglassProtocol.voiceAck(completedSession, completedBytes, 0, 0))
            complete(completedSession, target)
        } catch (failure: Throwable) {
            target.fill(0)
            throw failure
        }
        return true
    }

    private fun cancel(request: NightglassProtocol.VoiceRequest.Cancel): Boolean {
        val receiving = request.sessionId == session && buffer != null
        if (receiving) reset()
        // Cancellation remains meaningful after End has handed the owned copy to
        // OpenClaw. The service fences this callback against its current turn.
        cancelled(request.sessionId)
        return receiving
    }

    private fun reset() {
        buffer?.fill(0)
        buffer = null
        clearState()
    }

    private fun clearState() {
        session = 0u
        expectedBytes = 0
        expectedCrc = 0u
        received = 0
        nextSequence = 1
        chunksSinceAck = 0
    }

    companion object {
        fun crc32(data: ByteArray): UInt {
            val crc = CRC32()
            crc.update(data)
            return crc.value.toUInt()
        }
    }
}
