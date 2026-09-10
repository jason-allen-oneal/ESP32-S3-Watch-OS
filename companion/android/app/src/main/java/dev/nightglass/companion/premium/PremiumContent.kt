package dev.nightglass.companion.premium

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.Normalizer
import java.util.zip.CRC32

data class PremiumRequest(val token: UInt, val kind: Int, val target: UInt)
data class PremiumAction(val token: UInt, val action: Int, val index: Int, val target: UInt, val sequence: Int)
data class ContentAction(val action: Int, val index: Int, val label: String)
data class PremiumDocument(val kind: Int, val text: String, val actions: List<ContentAction> = emptyList(), val artwork: ByteArray? = null) {
    fun encode(): ByteArray {
        require(kind in 1..2 && actions.size <= 7 && (artwork == null || (kind == 1 && artwork.size == 3200)))
        val body = PremiumContent.ascii(text, if (artwork == null) 1600 else 400).toByteArray(Charsets.US_ASCII)
        require(body.isNotEmpty())
        val output = ByteArrayOutputStream()
        output.write(byteArrayOf(1, kind.toByte(), actions.size.toByte(), if (artwork == null) 0 else 1,
            body.size.toByte(), (body.size shr 8).toByte()))
        output.write(body)
        actions.forEach { a ->
            require(if (kind == 1) (a.action == 1 && a.index in 0..3) || (a.action in 2..4 && a.index == 0)
                else (a.action == 5 && a.index in 0..3) || (a.action == 6 && a.index == 0))
            val label = PremiumContent.ascii(a.label, 60).replace('\n', ' ').toByteArray(Charsets.US_ASCII)
            require(label.isNotEmpty())
            output.write(byteArrayOf(a.action.toByte(), a.index.toByte(), label.size.toByte())); output.write(label)
        }
        artwork?.let(output::write)
        return output.toByteArray().also { require(it.size <= 4096) }
    }
}
object PremiumContent {
    fun actionCaption(title: String): String = when (title.trim()) {
        "👍" -> "React: thumbs up"
        "👎" -> "React: thumbs down"
        "❤", "❤️" -> "React: heart"
        "😂" -> "React: laugh"
        "😮" -> "React: surprised"
        "😢" -> "React: sad"
        "🎉" -> "React: celebrate"
        else -> ascii(title, 60).replace('\n', ' ').trim()
    }
    fun spokenText(text: String): String {
        val marker = Regex("\\n\\[WATCH:(?:TIMER:([1-9][0-9]{1,3})|OPEN:(?:SPOTIFY|PHONE|INBOX)|PHONE:RING)\\]$")
            .find(text) ?: return text
        if (text.indexOf("[WATCH:") != marker.range.first + 1) return text
        val seconds = marker.groupValues[1]
        if (seconds.isNotEmpty() && seconds.toInt() !in 60..3600) return text
        return text.substring(0, marker.range.first)
    }
    fun ascii(text: String, limit: Int): String = Normalizer.normalize(text, Normalizer.Form.NFKD)
        .replace(Regex("\\p{M}+"), "").map { if (it == '\n' || it.code in 32..126) it else ' ' }
        .joinToString("").take(limit).trim()
    fun request(frame: ByteArray): PremiumRequest? = runCatching {
        require(frame.size == 11 && frame[0] == 1.toByte() && frame[1] == 0x72.toByte())
        val b = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN); b.position(2)
        val token = b.int.toUInt(); val kind = b.get().toInt(); val target = b.int.toUInt()
        require(token != 0u && kind in 1..2 && (kind == 1 || target != 0u))
        PremiumRequest(token, kind, target)
    }.getOrNull()
    fun action(frame: ByteArray): PremiumAction? = runCatching {
        require(frame.size == 13 && frame[0] == 1.toByte() && frame[1] == 0x76.toByte())
        val b = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN); b.position(2)
        val token = b.int.toUInt(); val action = b.get().toInt(); val index = b.get().toInt()
        val target = b.int.toUInt(); val sequence = b.get().toInt() and 255
        require(token != 0u && action in 1..6 && index in 0..3 && sequence != 0)
        PremiumAction(token, action, index, target, sequence)
    }.getOrNull()
    fun frames(request: PremiumRequest, document: PremiumDocument, maximumPayload: Int): List<ByteArray> {
        require(maximumPayload in 20..244 && request.kind == document.kind && request.token != 0u)
        val bytes = document.encode()
        val result = ArrayList<ByteArray>()
        result += ByteBuffer.allocate(13).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(1); put(0x73); putInt(request.token.toInt()); put(request.kind.toByte()); putInt(request.target.toInt()); putShort(bytes.size.toShort())
        }.array()
        var offset = 0
        while (offset < bytes.size) {
            val count = minOf(maximumPayload - 8, bytes.size - offset)
            result += ByteBuffer.allocate(8 + count).order(ByteOrder.LITTLE_ENDIAN).apply {
                put(1); put(0x74); putInt(request.token.toInt()); putShort(offset.toShort()); put(bytes, offset, count)
            }.array()
            offset += count
        }
        result += ByteBuffer.allocate(10).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(1); put(0x75); putInt(request.token.toInt()); putInt(CRC32().apply { update(bytes) }.value.toInt())
        }.array()
        bytes.fill(0)
        return result
    }
    fun result(action: PremiumAction, status: Int) = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).apply {
        require(status in 0..5)
        put(1); put(0x77); putInt(action.token.toInt()); put(action.sequence.toByte()); put(status.toByte())
    }.array()
}

/** Exact action ownership; expiry and sequence are checked before invoking any provider. */
class PremiumActionFence {
    private var request: PremiumRequest? = null
    private var actions = emptyList<ContentAction>()
    private var expiresAt = 0L
    private var sequence = 0
    @Synchronized fun bind(owner: PremiumRequest, offered: List<ContentAction>, now: Long) {
        if (request == owner) return // A retransmission cannot reset replay/expiry state.
        request = owner; actions = offered.toList(); expiresAt = now + 60_000; sequence = 0
    }
    @Synchronized fun clear() { request = null; actions = emptyList(); expiresAt = 0; sequence = 0 }
    @Synchronized fun accept(action: PremiumAction, now: Long): Boolean {
        val owner = request ?: return false
        val delta = (action.sequence - sequence) and 255
        if (now >= expiresAt || action.token != owner.token || action.target != owner.target ||
            delta == 0 || (sequence != 0 && delta >= 128) ||
            actions.none { it.action == action.action && it.index == action.index }) return false
        sequence = action.sequence
        return true
    }
}
