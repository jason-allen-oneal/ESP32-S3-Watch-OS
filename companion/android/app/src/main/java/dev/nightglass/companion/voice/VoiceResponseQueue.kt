package dev.nightglass.companion.voice

import dev.nightglass.companion.protocol.NightglassProtocol
import java.text.Normalizer
import java.util.ArrayDeque

data class VoiceTurnOwner(
    val watchSession: UInt,
    val linkGeneration: Long,
    val turnGeneration: Long,
)

/** Correlates Gateway events to one uncancelled watch/link/turn owner. */
class VoiceEventFence(private val owner: VoiceTurnOwner) {
    enum class Decision { ACCEPT, DROP, GAP }

    private var relaySession: String? = null
    private var turnId: String? = null
    private var lastSequence: Long? = null
    private var cancelled = false

    @Synchronized fun bindRelay(value: String) {
        require(value.isNotBlank() && relaySession == null)
        relaySession = value
    }

    @Synchronized fun cancel() { cancelled = true }

    @Synchronized fun accept(
        candidateOwner: VoiceTurnOwner,
        candidateRelay: String?,
        candidateTurn: String?,
        sequence: Long?,
    ): Decision {
        if (cancelled || candidateOwner != owner || candidateRelay.isNullOrBlank() ||
            candidateRelay != relaySession) return Decision.DROP
        if (sequence != null) {
            val previous = lastSequence
            if (previous != null && sequence <= previous) return Decision.DROP
            lastSequence = sequence
        }
        if (!candidateTurn.isNullOrBlank()) {
            if (turnId == null) turnId = candidateTurn
            else if (turnId != candidateTurn) return Decision.DROP
        }
        return Decision.ACCEPT
    }
}

/** Dedicated, atomic, GATT-write-acknowledged watch response transport. */
class VoiceResponseQueue(private val capacity: Int = 768) {
    data class Entry(
        val frame: ByteArray,
        val owner: VoiceTurnOwner?,
        val terminal: Boolean,
    )

    private val queued = ArrayDeque<Entry>()
    private var pending: Entry? = null

    @Synchronized fun enqueueControl(frame: ByteArray, maximumPayload: Int): Boolean {
        if (frame.isEmpty() || frame.size > maximumPayload || sizeLocked() >= capacity) return false
        queued.add(Entry(frame.copyOf(), null, false))
        return true
    }

    @Synchronized fun enqueueResponse(
        owner: VoiceTurnOwner,
        responseId: UInt,
        text: String,
        maximumPayload: Int,
    ): Boolean {
        if (responseId == 0u || maximumPayload < 16 || hasOwnerLocked()) return false
        val payload = asciiForWatch(text)
        if (payload.isEmpty()) return false
        val maximumChunk = minOf(232, maximumPayload - 12)
        if (maximumChunk <= 0) {
            payload.fill(0)
            return false
        }
        val crc = VoiceTransferReceiver.crc32(payload)
        val frames = ArrayList<Entry>(2 + (payload.size + maximumChunk - 1) / maximumChunk)
        frames.add(Entry(NightglassProtocol.voiceResponseBegin(
            owner.watchSession, responseId, payload.size, crc), owner, false))
        var offset = 0
        while (offset < payload.size) {
            val end = minOf(offset + maximumChunk, payload.size)
            val chunk = payload.copyOfRange(offset, end)
            frames.add(Entry(NightglassProtocol.voiceResponseData(
                owner.watchSession, responseId, offset, chunk), owner, false))
            chunk.fill(0)
            offset = end
        }
        frames.add(Entry(NightglassProtocol.voiceResponseEnd(
            owner.watchSession, responseId, payload.size, crc), owner, true))
        payload.fill(0)
        if (sizeLocked() + frames.size > capacity || frames.any { it.frame.size > maximumPayload }) {
            frames.forEach { it.frame.fill(0) }
            return false
        }
        frames.forEach(queued::add)
        return true
    }

    /** Atomically queues text followed by an optional bounded spoken reply. */
    @Synchronized fun enqueueResponseWithAudio(
        owner: VoiceTurnOwner,
        responseId: UInt,
        audioResponseId: UInt,
        text: String,
        audio: ByteArray,
        maximumPayload: Int,
    ): Boolean {
        if (responseId == 0u || audioResponseId == 0u || responseId == audioResponseId ||
            maximumPayload < 20 || hasOwnerLocked() ||
            audio.isEmpty() || audio.size > NightglassProtocol.MAX_SPOKEN_REPLY_BYTES) return false
        val payload = asciiForWatch(text)
        if (payload.isEmpty()) return false
        val maximumChunk = minOf(232, maximumPayload - 12)
        if (maximumChunk <= 0) {
            payload.fill(0)
            return false
        }
        val textCrc = VoiceTransferReceiver.crc32(payload)
        val audioCrc = VoiceTransferReceiver.crc32(audio)
        val frames = ArrayList<Entry>(
            5 + (payload.size + maximumChunk - 1) / maximumChunk +
                (audio.size + maximumChunk - 1) / maximumChunk)
        frames.add(Entry(NightglassProtocol.voiceResponseBegin(
            owner.watchSession, responseId, payload.size, textCrc), owner, false))
        var offset = 0
        while (offset < payload.size) {
            val end = minOf(offset + maximumChunk, payload.size)
            val chunk = payload.copyOfRange(offset, end)
            frames.add(Entry(NightglassProtocol.voiceResponseData(
                owner.watchSession, responseId, offset, chunk), owner, false))
            chunk.fill(0)
            offset = end
        }
        frames.add(Entry(NightglassProtocol.voiceResponseEnd(
            owner.watchSession, responseId, payload.size, textCrc), owner, false))
        frames.add(Entry(NightglassProtocol.voiceAudioResponseBegin(
            owner.watchSession, audioResponseId, audio.size, audioCrc), owner, false))
        val audioMaximumChunk = minOf(230, maximumPayload - 14)
        if (audioMaximumChunk <= 0) {
            payload.fill(0)
            frames.forEach { it.frame.fill(0) }
            return false
        }
        offset = 0
        while (offset < audio.size) {
            val end = minOf(offset + audioMaximumChunk, audio.size)
            val chunk = audio.copyOfRange(offset, end)
            frames.add(Entry(NightglassProtocol.voiceAudioResponseData(
                owner.watchSession, audioResponseId, offset, chunk), owner, false))
            chunk.fill(0)
            offset = end
        }
        frames.add(Entry(NightglassProtocol.voiceAudioResponseEnd(
            owner.watchSession, audioResponseId, audio.size, audioCrc), owner, true))
        payload.fill(0)
        if (sizeLocked() + frames.size > capacity || frames.any { it.frame.size > maximumPayload }) {
            frames.forEach { it.frame.fill(0) }
            return false
        }
        frames.forEach(queued::add)
        return true
    }

    @Synchronized fun beginWrite(): Entry? {
        if (pending != null) return null
        return queued.poll()?.also { pending = it }
    }

    @Synchronized fun completeWrite(): VoiceTurnOwner? {
        val completed = pending ?: return null
        pending = null
        completed.frame.fill(0)
        return completed.owner?.takeIf { completed.terminal }
    }

    @Synchronized fun failWrite() {
        pending?.frame?.fill(0)
        pending = null
        resetLocked()
    }

    @Synchronized fun purge(owner: VoiceTurnOwner? = null) {
        if (owner == null || pending?.owner == owner) {
            pending?.frame?.fill(0)
            pending = null
        }
        val keep = ArrayDeque<Entry>()
        while (queued.isNotEmpty()) {
            val entry = queued.removeFirst()
            if (owner == null || entry.owner == owner || entry.owner == null) entry.frame.fill(0)
            else keep.add(entry)
        }
        queued.addAll(keep)
    }

    @Synchronized fun reset() = resetLocked()

    @Synchronized fun size(): Int = sizeLocked()

    private fun hasOwnerLocked() = pending?.owner != null || queued.any { it.owner != null }
    private fun sizeLocked() = queued.size + if (pending == null) 0 else 1
    private fun resetLocked() {
        pending?.frame?.fill(0)
        pending = null
        queued.forEach { it.frame.fill(0) }
        queued.clear()
    }

    companion object {
        private const val MAXIMUM_TEXT_BYTES = 2048

        /** Printable ASCII plus newline, matching the watch-side parser exactly. */
        fun asciiForWatch(input: String): ByteArray {
            val punctuation = input
                .replace('\u2018', '\'').replace('\u2019', '\'')
                .replace('\u201c', '"').replace('\u201d', '"')
                .replace("\u2013", "-").replace("\u2014", "-")
                .replace("\u2026", "...")
            val decomposed = Normalizer.normalize(punctuation, Normalizer.Form.NFKD)
            val result = ByteArray(MAXIMUM_TEXT_BYTES)
            var length = 0
            var previousSpace = false
            decomposed.forEach { character ->
                if (length >= result.size) return@forEach
                val code = character.code
                when {
                    character == '\n' -> {
                        while (length > 0 && result[length - 1] == ' '.code.toByte()) length--
                        if (length > 0 && result[length - 1] != '\n'.code.toByte()) {
                            result[length++] = '\n'.code.toByte()
                        }
                        previousSpace = false
                    }
                    code == '\t'.code || code == ' '.code -> if (!previousSpace && length > 0) {
                        result[length++] = ' '.code.toByte()
                        previousSpace = true
                    }
                    code in 0x21..0x7e -> {
                        result[length++] = code.toByte()
                        previousSpace = false
                    }
                    Character.getType(character) == Character.NON_SPACING_MARK.toInt() -> Unit
                    else -> if (length == 0 || result[length - 1] != '?'.code.toByte()) {
                        result[length++] = '?'.code.toByte()
                        previousSpace = false
                    }
                }
            }
            while (length > 0 && (result[length - 1] == ' '.code.toByte() ||
                    result[length - 1] == '\n'.code.toByte())) length--
            return result.copyOf(length).also { result.fill(0) }
        }
    }
}
