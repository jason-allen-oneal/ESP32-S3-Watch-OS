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

    data class RelayNotification(val id: UInt, val category: Int, val app: String,
                                 val title: String, val body: String,
                                 val replyable: Boolean = false)
    sealed interface WatchAction {
        data class Media(val sequence: Int, val command: Int): WatchAction
        data class Notification(val sequence: Int, val id: UInt, val dismiss: Boolean): WatchAction
        data class Reply(val sequence: Int, val id: UInt, val nonce: UInt,
                         val text: String): WatchAction
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
                   available: Boolean): ByteArray {
        val title = ascii(titleValue, 48); val artist = ascii(artistValue, 48)
        val flags = (if (playing) 1 else 0) or (if (available) 2 else 0)
        return ByteBuffer.allocate(5 + title.size + artist.size)
            .put(VERSION).put(4).put(flags.toByte()).put(title.size.toByte())
            .put(artist.size.toByte()).put(title).put(artist).array()
    }
    fun parseAction(frame: ByteArray): WatchAction? {
        if (frame.size < 2 || frame[0] != VERSION) return null
        return when (frame[1].toInt() and 0xff) {
            0x10 -> if (frame.size == 4) WatchAction.Media(frame[2].toInt() and 0xff, frame[3].toInt() and 0xff) else null
            0x11, 0x12 -> if (frame.size == 7) WatchAction.Notification(frame[2].toInt() and 0xff, ByteBuffer.wrap(frame, 3, 4).order(ByteOrder.LITTLE_ENDIAN).int.toUInt(), frame[1].toInt() == 0x11) else null
            0x13 -> {
                if (frame.size < 13) null else {
                    val length = frame[11].toInt() and 0xff
                    val nonce = ByteBuffer.wrap(frame, 7, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).int.toUInt()
                    val id = ByteBuffer.wrap(frame, 3, 4)
                        .order(ByteOrder.LITTLE_ENDIAN).int.toUInt()
                    val textBytes = frame.copyOfRange(12, frame.size)
                    if (id == 0u || nonce == 0u || length !in 1..96 ||
                        frame.size != 12 + length ||
                        textBytes.any { (it.toInt() and 0xff) !in 0x20..0x7e }) null
                    else WatchAction.Reply(
                        frame[2].toInt() and 0xff,
                        id,
                        nonce,
                        textBytes.toString(Charsets.US_ASCII))
                }
            }
            else -> null
        }
    }
    fun replyResult(sequence: Int, status: Int, id: UInt, nonce: UInt): ByteArray {
        require(status in 0..5 && id != 0u && nonce != 0u)
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
