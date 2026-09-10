package dev.nightglass.companion.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

object NightglassProtocol {
    val SERVICE: UUID = UUID.fromString("7a3b4001-6b6f-4f72-726f-772d6e696768")
    val STATUS: UUID = UUID.fromString("7a3b4002-6b6f-4f72-726f-772d6e696768")
    val PHONE_TO_WATCH: UUID = UUID.fromString("7a3b4003-6b6f-4f72-726f-772d6e696768")
    val WATCH_TO_PHONE: UUID = UUID.fromString("7a3b4004-6b6f-4f72-726f-772d6e696768")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    const val VERSION: Byte = 1
    /** Five minutes of 8 kHz G.711 mu-law audio. */
    const val MAX_VOICE_ENCODED_BYTES = 2_400_000
    const val MAX_SPOKEN_REPLY_BYTES = 96_000
    const val DEFAULT_VOICE_DURATION_SECONDS = 60
    const val MAX_DISCORD_VOICE_REPLY_BYTES = DEFAULT_VOICE_DURATION_SECONDS * 8_000
    const val MAX_VOICE_DURATION_SECONDS = 300
    const val VOICE_REQUEST_FLAG_DISCORD_REPLY = 0x40
    const val VOICE_REQUEST_FLAG_SPOKEN_REPLIES = 0x80
    private const val VOICE_REQUEST_CODEC_MASK = 0x3f

    fun voiceDurationAllowed(seconds: Int): Boolean =
        seconds == 30 || seconds == 60 || seconds == 120 || seconds == 300
    fun nextNonzeroSequence32(current: UInt): UInt =
        if (current == UInt.MAX_VALUE) 1u else current + 1u
    private const val CONNECTED_ENCRYPTED = 3

    data class WatchStatus(val state: Int, val notificationCount: Int,
                           val encrypted: Boolean, val bonded: Boolean,
                           val peerIdentityPinned: Boolean) {
        val authorized: Boolean
            get() = state == CONNECTED_ENCRYPTED && encrypted && bonded &&
                peerIdentityPinned
    }

    fun parseWatchStatus(frame: ByteArray): WatchStatus? {
        if (frame.size != 6 || frame[0] != VERSION) return null
        val state = frame[1].toInt() and 0xff
        val notificationCount = frame[2].toInt() and 0xff
        val flags = frame.copyOfRange(3, 6).map { it.toInt() and 0xff }
        if (state !in 0..4 || notificationCount > 6 || flags.any { it !in 0..1 }) return null
        return WatchStatus(state, notificationCount, flags[0] != 0, flags[1] != 0,
            flags[2] != 0)
    }

    /** Wrap-safe 16-bit replay window; accepts only the next forward half-space. */
    fun acceptsForwardSequence(last: Int, candidate: Int): Boolean {
        if (last !in 0..0xffff || candidate !in 1..0xffff) return false
        if (last == 0) return true
        val delta = (candidate - last) and 0xffff
        return delta in 1..0x7fff
    }

    fun acceptsForwardSequence8(last: Int, candidate: Int): Boolean {
        if (last !in 0..0xff || candidate !in 1..0xff) return false
        if (last == 0) return true
        val delta = (candidate - last) and 0xff
        return delta in 1..0x7f
    }

    class CallCommandWindow {
        private var sessionId = 0u
        private var generation = 0
        private var lastSequence = 0

        @Synchronized fun updateSession(sessionId: UInt, generation: Int) {
            require((sessionId == 0u && generation == 0) ||
                (sessionId != 0u && generation in 1..0xffff))
            if (this.sessionId != sessionId || this.generation != generation) {
                this.sessionId = sessionId
                this.generation = generation
                lastSequence = 0
            }
        }

        @Synchronized fun accept(action: WatchAction.Call): Boolean {
            if (sessionId == 0u || action.command !in 1..4 ||
                action.sessionId != sessionId ||
                action.generation != generation ||
                !acceptsForwardSequence(lastSequence, action.sequence)) return false
            lastSequence = action.sequence
            return true
        }
    }

    data class RelayNotification(val id: UInt, val category: Int, val app: String,
                                 val title: String, val body: String,
                                 val replyable: Boolean = false)
    data class AgendaEvent(val startEpochSeconds: Long, val endEpochSeconds: Long,
                           val title: String, val location: String, val allDay: Boolean)
    data class OtaManifest(val formatVersion: Long, val boardId: String,
                           val partitionId: String, val partitionRevision: Long,
                           val appVersion: String, val secureVersion: Long,
                           val imageSize: Long, val imageSha256: ByteArray,
                           val signature: ByteArray)
    data class OtaStatus(val session: ULong, val state: Int, val signatureState: Int,
                         val result: Int, val expectedBytes: Long,
                         val receivedBytes: Long,
                         val acknowledgedOpcode: Int = 0)
    sealed interface VoiceRequest {
        val sessionId: UInt
        data class Begin(override val sessionId: UInt, val totalBytes: Int,
                         val crc32: UInt,
                         val spokenReplies: Boolean = false,
                         val discordReply: Boolean = false): VoiceRequest
        data class Data(override val sessionId: UInt, val sequence: Int,
                        val offset: Int, val payload: ByteArray): VoiceRequest
        data class End(override val sessionId: UInt, val totalBytes: Int,
                       val crc32: UInt): VoiceRequest
        data class Cancel(override val sessionId: UInt, val reason: Int): VoiceRequest
    }
    sealed interface WatchAction {
        data class Media(val sequence: Int, val command: Int): WatchAction
        data class Notification(val sequence: Int, val id: UInt, val dismiss: Boolean): WatchAction
        data class Reply(val sequence: Int, val id: UInt, val nonce: UInt,
                         val text: String): WatchAction
        data class Call(val sequence: Int, val command: Int, val sessionId: UInt,
                        val generation: Int): WatchAction
        data class Phone(val sequence: Int, val command: Int): WatchAction
    }

    private fun ascii(value: String, max: Int) = value.map { if (it.code in 0x20..0x7e) it else '?' }.joinToString("").toByteArray(Charsets.US_ASCII).copyOfRange(0, minOf(max, value.length))
    fun upsert(n: RelayNotification, alert: Boolean = true): ByteArray {
        val app = ascii(n.app, 24); val title = ascii(n.title, 48); val body = ascii(n.body, 96)
        val category = n.category.coerceIn(0, 5) or (if (n.replyable) 0x40 else 0) or
            (if (alert) 0x80 else 0)
        return ByteBuffer.allocate(11 + app.size + title.size + body.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(1).putInt(n.id.toInt()).put(category.toByte()).put(app.size.toByte()).put(title.size.toByte()).putShort(body.size.toShort()).put(app).put(title).put(body).array()
    }
    fun remove(id: UInt) = ByteBuffer.allocate(6).order(ByteOrder.LITTLE_ENDIAN).put(VERSION).put(2).putInt(id.toInt()).array()
    fun clear() = byteArrayOf(VERSION, 3)
    fun mediaState(titleValue: String, artistValue: String, playing: Boolean,
                   available: Boolean, seekable: Boolean = false,
                   positionMs: Long = 0, durationMs: Long = 0): ByteArray {
        val title = ascii(titleValue, 48); val artist = ascii(artistValue, 48)
        require(positionMs in 0..604_800_000L && durationMs in 0..604_800_000L)
        require(durationMs == 0L || positionMs <= durationMs)
        require(available || (!playing && !seekable && positionMs == 0L && durationMs == 0L &&
            title.isEmpty() && artist.isEmpty()))
        val flags = (if (playing) 1 else 0) or (if (available) 2 else 0) or
            (if (seekable) 4 else 0)
        return ByteBuffer.allocate(13 + title.size + artist.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(7).put(flags.toByte()).put(title.size.toByte())
            .put(artist.size.toByte()).putInt(positionMs.toInt()).putInt(durationMs.toInt())
            .put(title).put(artist).array()
    }
    fun agenda(events: List<AgendaEvent>): ByteArray {
        require(events.size <= 3)
        val encoded = events.map { event ->
            require(event.startEpochSeconds in 1_577_836_800L..UInt.MAX_VALUE.toLong())
            require(event.endEpochSeconds in event.startEpochSeconds..UInt.MAX_VALUE.toLong())
            Triple(ascii(event.title, 32), ascii(event.location, 15), event)
        }
        val size = 3 + encoded.sumOf { 11 + it.first.size + it.second.size }
        require(size <= 179)
        return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(VERSION).put(5).put(encoded.size.toByte())
            encoded.forEach { (title, location, event) ->
                putInt(event.startEpochSeconds.toInt()).putInt(event.endEpochSeconds.toInt())
                put((if (event.allDay) 1 else 0).toByte()).put(title.size.toByte())
                    .put(location.size.toByte()).put(title).put(location)
            }
        }.array()
    }
    fun phoneBattery(percent: Int, charging: Boolean, powerSave: Boolean): ByteArray {
        require(percent in 0..100)
        val flags = (if (charging) 1 else 0) or (if (powerSave) 2 else 0)
        return byteArrayOf(VERSION, 6, percent.toByte(), flags.toByte(), 0)
    }
    fun callState(ringing: Boolean, active: Boolean, muted: Boolean,
                  canAnswer: Boolean, canReject: Boolean, sessionId: UInt,
                  generation: Int, labelValue: String): ByteArray {
        require(!canAnswer || ringing)
        val idle = !ringing && !active
        require((idle && sessionId == 0u && generation == 0) ||
            (!idle && sessionId != 0u && generation in 1..0xffff))
        val label = ascii(labelValue, 48)
        val flags = (if (ringing) 1 else 0) or (if (active) 2 else 0) or
            (if (muted) 4 else 0) or (if (canAnswer) 8 else 0) or
            (if (canReject) 16 else 0)
        return ByteBuffer.allocate(10 + label.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(8).put(flags.toByte()).put(label.size.toByte())
            .putInt(sessionId.toInt()).putShort(generation.toShort()).put(label).array()
    }
    fun parseAction(frame: ByteArray): WatchAction? {
        if (frame.size < 2 || frame[0] != VERSION) return null
        return when (frame[1].toInt() and 0xff) {
            0x10 -> if (frame.size == 4 && (frame[2].toInt() and 0xff) != 0 &&
                (frame[3].toInt() and 0xff) in 1..9)
                WatchAction.Media(frame[2].toInt() and 0xff, frame[3].toInt() and 0xff)
                else null
            0x11, 0x12 -> if (frame.size == 7 && (frame[2].toInt() and 0xff) != 0) {
                val id = ByteBuffer.wrap(frame, 3, 4).order(ByteOrder.LITTLE_ENDIAN).int.toUInt()
                if (id != 0u) WatchAction.Notification(frame[2].toInt() and 0xff, id,
                    frame[1].toInt() == 0x11) else null
            } else null
            0x13 -> {
                if (frame.size < 13) null else {
                    val length = frame[11].toInt() and 0xff
                    val nonce = ByteBuffer.wrap(frame, 7, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).int.toUInt()
                    val id = ByteBuffer.wrap(frame, 3, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).int.toUInt()
                    val textBytes = frame.copyOfRange(12, frame.size)
                    if ((frame[2].toInt() and 0xff) == 0 || id == 0u || nonce == 0u || length !in 1..96 ||
                        frame.size != 12 + length ||
                        textBytes.any { (it.toInt() and 0xff) !in 0x20..0x7e }) null
                    else WatchAction.Reply(
                        frame[2].toInt() and 0xff,
                        id,
                        nonce,
                        textBytes.toString(Charsets.US_ASCII))
                }
            }
            0x14 -> if (frame.size == 11) {
                val sequence = ByteBuffer.wrap(frame, 2, 2).order(ByteOrder.LITTLE_ENDIAN)
                    .short.toInt() and 0xffff
                val command = frame[4].toInt() and 0xff
                val sessionId = ByteBuffer.wrap(frame, 5, 4).order(ByteOrder.LITTLE_ENDIAN)
                    .int.toUInt()
                val generation = ByteBuffer.wrap(frame, 9, 2).order(ByteOrder.LITTLE_ENDIAN)
                    .short.toInt() and 0xffff
                if (sequence != 0 && command in 1..4 && sessionId != 0u && generation != 0)
                    WatchAction.Call(sequence, command, sessionId, generation) else null
            } else null
            0x15 -> if (frame.size == 4 && (frame[2].toInt() and 0xff) != 0 &&
                (frame[3].toInt() and 0xff) in 1..5)
                WatchAction.Phone(frame[2].toInt() and 0xff, frame[3].toInt() and 0xff) else null
            else -> null
        }
    }

    fun parseVoiceRequest(frame: ByteArray): VoiceRequest? {
        if (frame.size < 7 || frame[0] != VERSION) return null
        val input = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN)
        input.position(2)
        val session = input.int.toUInt()
        if (session == 0u) return null
        return when (frame[1].toInt() and 0xff) {
            0x40 -> if (frame.size == 16) {
                val total = input.int.toUInt().toLong()
                val crc = input.int.toUInt()
                val codecByte = input.get().toInt() and 0xff
                val codec = codecByte and VOICE_REQUEST_CODEC_MASK
                val rateKhz = input.get().toInt() and 0xff
                val spoken = (codecByte and VOICE_REQUEST_FLAG_SPOKEN_REPLIES) != 0
                val discord = (codecByte and VOICE_REQUEST_FLAG_DISCORD_REPLY) != 0
                if (total in 1..MAX_VOICE_ENCODED_BYTES.toLong() &&
                    (!discord || total <= MAX_DISCORD_VOICE_REPLY_BYTES.toLong()) &&
                    codec == 1 && rateKhz == 8)
                    VoiceRequest.Begin(session, total.toInt(), crc, spoken, discord) else null
            } else null
            0x41 -> if (frame.size in 13..244) {
                val sequence = input.short.toInt() and 0xffff
                val offset = input.int.toUInt().toLong()
                val payload = frame.copyOfRange(12, frame.size)
                if (sequence != 0 && offset in 0..MAX_VOICE_ENCODED_BYTES.toLong() &&
                    payload.isNotEmpty() && payload.size <= 232 &&
                    payload.size <= MAX_VOICE_ENCODED_BYTES.toLong() - offset)
                    VoiceRequest.Data(session, sequence, offset.toInt(), payload) else null
            } else null
            0x42 -> if (frame.size == 14) {
                val total = input.int.toUInt().toLong()
                val crc = input.int.toUInt()
                if (total in 1..MAX_VOICE_ENCODED_BYTES.toLong())
                    VoiceRequest.End(session, total.toInt(), crc)
                else null
            } else null
            0x43 -> if (frame.size == 7) {
                val reason = input.get().toInt() and 0xff
                if (reason in 1..8) VoiceRequest.Cancel(session, reason) else null
            } else null
            else -> null
        }
    }

    fun voiceAck(session: UInt, nextOffset: Int, credits: Int, status: Int): ByteArray {
        require(session != 0u && nextOffset in 0..MAX_VOICE_ENCODED_BYTES &&
            credits in 0..8 && status in 0..8)
        return ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x44).putInt(session.toInt()).putInt(nextOffset)
            .put(credits.toByte()).put(status.toByte()).array()
    }

    fun voiceResponseBegin(session: UInt, responseId: UInt, totalBytes: Int,
                           crc32: UInt): ByteArray {
        require(session != 0u && responseId != 0u && totalBytes in 1..2048)
        return ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x45).putInt(session.toInt()).putInt(responseId.toInt())
            .putShort(totalBytes.toShort()).putInt(crc32.toInt()).array()
    }

    fun voiceResponseData(session: UInt, responseId: UInt, offset: Int,
                          payload: ByteArray): ByteArray {
        require(session != 0u && responseId != 0u && offset in 0..2048 &&
            payload.isNotEmpty() && payload.size <= 232 && payload.size <= 2048 - offset)
        return ByteBuffer.allocate(12 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x46).putInt(session.toInt()).putInt(responseId.toInt())
            .putShort(offset.toShort()).put(payload).array()
    }

    fun voiceResponseEnd(session: UInt, responseId: UInt, totalBytes: Int,
                         crc32: UInt): ByteArray {
        require(session != 0u && responseId != 0u && totalBytes in 1..2048)
        return ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x47).putInt(session.toInt()).putInt(responseId.toInt())
            .putShort(totalBytes.toShort()).putInt(crc32.toInt()).array()
    }

    fun voiceAudioResponseBegin(session: UInt, responseId: UInt, totalBytes: Int,
                                crc32: UInt): ByteArray {
        require(session != 0u && responseId != 0u &&
            totalBytes in 1..MAX_SPOKEN_REPLY_BYTES)
        return ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x4a).putInt(session.toInt()).putInt(responseId.toInt())
            .putInt(totalBytes).putInt(crc32.toInt()).put(1).put(8).array()
    }

    fun voiceAudioResponseData(session: UInt, responseId: UInt, offset: Int,
                               payload: ByteArray): ByteArray {
        require(session != 0u && responseId != 0u &&
            offset in 0..MAX_SPOKEN_REPLY_BYTES && payload.isNotEmpty() &&
            payload.size <= 232 && payload.size <= MAX_SPOKEN_REPLY_BYTES - offset)
        return ByteBuffer.allocate(14 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x4b).putInt(session.toInt()).putInt(responseId.toInt())
            .putInt(offset).put(payload).array()
    }

    fun voiceAudioResponseEnd(session: UInt, responseId: UInt, totalBytes: Int,
                              crc32: UInt): ByteArray {
        require(session != 0u && responseId != 0u &&
            totalBytes in 1..MAX_SPOKEN_REPLY_BYTES)
        return ByteBuffer.allocate(18).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x4c).putInt(session.toInt()).putInt(responseId.toInt())
            .putInt(totalBytes).putInt(crc32.toInt()).array()
    }

    fun voiceStatus(session: UInt, status: Int): ByteArray {
        require(session != 0u && status in 0..8)
        return ByteBuffer.allocate(7).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x48).putInt(session.toInt()).put(status.toByte()).array()
    }
    fun voiceHealth(sequence: UInt, state: Int): ByteArray {
        require(sequence != 0u && state in 0..2)
        return ByteBuffer.allocate(7).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x49).putInt(sequence.toInt()).put(state.toByte()).array()
    }
    fun replyResult(sequence: Int, status: Int, id: UInt, nonce: UInt): ByteArray {
        require(sequence in 1..0xff && status in 0..5 && id != 0u && nonce != 0u)
        return ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x24).put(sequence.toByte()).put(status.toByte())
            .putInt(id.toInt()).putInt(nonce.toInt()).array()
    }

    fun provisionWifi(ssid: String, passphrase: CharArray): ByteArray {
        val s = ascii(ssid, 32); val p = ascii(passphrase.concatToString(), 64)
        require(s.isNotEmpty())
        return ByteBuffer.allocate(4 + s.size + p.size).put(VERSION).put(0x20).put(s.size.toByte()).put(p.size.toByte()).put(s).put(p).array()
    }
    fun configureWeather(enabled: Boolean, locationConfigured: Boolean, metric: Boolean, refreshMinutes: Int, latitudeE6: Int, longitudeE6: Int): ByteArray {
        require(refreshMinutes in 15..360 && latitudeE6 in -90_000_000..90_000_000 && longitudeE6 in -180_000_000..180_000_000)
        val flags = (if (enabled) 1 else 0) or (if (locationConfigured) 2 else 0)
        return ByteBuffer.allocate(14).order(ByteOrder.LITTLE_ENDIAN).put(VERSION).put(0x21).put(flags.toByte()).put(if (metric) 1 else 0).putShort(refreshMinutes.toShort()).putInt(latitudeE6).putInt(longitudeE6).array()
    }
    fun clearWifi() = byteArrayOf(VERSION, 0x22)
    fun forgetPeerAuthorization() = byteArrayOf(VERSION, 0x25)

    private fun canonicalOtaText(value: String, maximum: Int): ByteArray {
        val bytes = value.toByteArray(Charsets.US_ASCII)
        require(bytes.isNotEmpty() && bytes.size <= maximum)
        require(bytes.all { (it.toInt() and 0xff) in 0x21..0x7e && it != '\\'.code.toByte() &&
            it != '='.code.toByte() })
        return bytes
    }

    fun otaBegin(session: ULong, manifest: OtaManifest): ByteArray {
        require(session != 0uL && manifest.formatVersion in 1..UInt.MAX_VALUE.toLong())
        require(manifest.partitionRevision in 1..UInt.MAX_VALUE.toLong())
        require(manifest.secureVersion in 0..UInt.MAX_VALUE.toLong())
        require(manifest.imageSize in 1..6L * 1024 * 1024)
        require(manifest.imageSha256.size == 32)
        require(manifest.signature.size in 8..72)
        val board = canonicalOtaText(manifest.boardId, 47)
        val partition = canonicalOtaText(manifest.partitionId, 31)
        val version = canonicalOtaText(manifest.appVersion, 31)
        val size = 62 + board.size + partition.size + version.size + manifest.signature.size
        require(size <= 244)
        return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x30).putLong(session.toLong())
            .putInt(manifest.formatVersion.toInt())
            .putInt(manifest.partitionRevision.toInt())
            .putInt(manifest.secureVersion.toInt()).putInt(manifest.imageSize.toInt())
            .put(manifest.imageSha256).put(board.size.toByte()).put(partition.size.toByte())
            .put(version.size.toByte()).put(manifest.signature.size.toByte())
            .put(board).put(partition).put(version).put(manifest.signature).array()
    }

    fun otaData(session: ULong, offset: Long, data: ByteArray): ByteArray {
        require(session != 0uL && offset in 0..UInt.MAX_VALUE.toLong() && data.size in 1..230)
        return ByteBuffer.allocate(14 + data.size).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x31).putLong(session.toLong()).putInt(offset.toInt())
            .put(data).array()
    }

    fun otaFinish(session: ULong) = otaControl(0x32, session)
    fun otaAbort(session: ULong) = otaControl(0x33, session)
    fun otaStatusQuery(session: ULong) = otaControl(0x34, session)
    private fun otaControl(opcode: Int, session: ULong): ByteArray {
        require(session != 0uL)
        return ByteBuffer.allocate(10).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(opcode.toByte()).putLong(session.toLong()).array()
    }

    fun parseOtaStatus(frame: ByteArray): OtaStatus? {
        if (frame.size != 22 || frame[0] != VERSION ||
            (frame[1].toInt() and 0xff) != 0x35) return null
        val input = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN)
        input.position(2)
        val session = input.long.toULong()
        val state = input.get().toInt() and 0xff
        val signatureState = input.get().toInt() and 0xff
        val result = input.get().toInt() and 0xff
        val acknowledgedOpcode = input.get().toInt() and 0xff
        val expected = input.int.toUInt().toLong()
        val received = input.int.toUInt().toLong()
        if (session == 0uL || state !in 0..4 || signatureState !in 0..4 ||
            result !in 0..9 ||
            (acknowledgedOpcode != 0 && acknowledgedOpcode !in 0x30..0x34) ||
            received > expected) return null
        return OtaStatus(session, state, signatureState, result, expected, received,
            acknowledgedOpcode)
    }

    fun phoneWeather(observedEpochSeconds: Long, metric: Boolean, isDay: Boolean,
                     temperature: Double, apparentTemperature: Double,
                     weatherCode: Int, windSpeed: Double): ByteArray {
        require(observedEpochSeconds in 1_577_836_800L..UInt.MAX_VALUE.toLong())
        require(temperature in -150.0..150.0 && apparentTemperature in -150.0..150.0)
        require(weatherCode in 0..999 && windSpeed in 0.0..500.0)
        val temperatureTenths = kotlin.math.round(temperature * 10).toInt()
        val apparentTenths = kotlin.math.round(apparentTemperature * 10).toInt()
        val windTenths = kotlin.math.round(windSpeed * 10).toInt()
        val flags = (if (metric) 1 else 0) or (if (isDay) 2 else 0)
        return ByteBuffer.allocate(18).order(ByteOrder.LITTLE_ENDIAN)
            .put(VERSION).put(0x23).put(flags.toByte()).put(0)
            .putInt(observedEpochSeconds.toInt())
            .putShort(0) // The phone fetched this observation immediately.
            .putShort(temperatureTenths.toShort())
            .putShort(apparentTenths.toShort())
            .putShort(weatherCode.toShort()).putShort(windTenths.toShort()).array()
    }
}
