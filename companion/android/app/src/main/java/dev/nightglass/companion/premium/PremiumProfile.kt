package dev.nightglass.companion.premium

import kotlinx.serialization.json.*
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** Portable appearance only. Pairing keys, Wi-Fi and Gateway credentials are never exported. */
data class PremiumProfile(
    val name: String = "My Nightglass",
    val face: Int = 1,
    val accent: Int = 0xa8ff32,
    val complications: List<Int> = listOf(1, 2, 0),
    val deckOrder: List<Int> = listOf(0, 1, 2, 3, 4),
    val deckMask: Int = 31,
    val alwaysOn: Boolean = false,
    val largeText: Boolean = false,
    val reduceMotion: Boolean = false,
) {
    val flags get() = (if (alwaysOn) 1 else 0) or (if (largeText) 2 else 0) or (if (reduceMotion) 4 else 0)
    fun validate(): PremiumProfile = apply {
        require(name.length in 1..24 && name.isNotBlank() && name.all { it.code in 32..126 }) { "Use 1–24 plain-text characters for the face name" }
        require(face in 0..2 && accent in 0..0xffffff) { "Unsupported face or color" }
        require((accent shr 16) * 3 + ((accent shr 8) and 255) * 6 + (accent and 255) >= 960) { "Choose a brighter accent for readable text" }
        require(complications.size == 3 && complications.all { it in 0..7 }) { "Choose three supported complications" }
        require(deckOrder.size == 5 && deckOrder.toSet() == (0..4).toSet()) { "Each card must appear once in the order" }
        require(deckMask in 1..31) { "Keep at least one Context card enabled" }
    }
    fun frame(nonce: UInt, response: Boolean = false): ByteArray {
        validate()
        return ByteBuffer.allocate(48).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(1); put(if (response) 0x71 else 0x70); putInt(nonce.toInt())
            put(face.toByte()); put(flags.toByte())
            put((accent shr 16).toByte()); put((accent shr 8).toByte()); put(accent.toByte())
            complications.forEach { put(it.toByte()) }; deckOrder.forEach { put(it.toByte()) }
            put(deckMask.toByte()); put(name.length.toByte()); put(name.toByteArray(Charsets.US_ASCII))
        }.array()
    }
    fun json(): String = Json { prettyPrint = true }.encodeToString(JsonObject.serializer(), buildJsonObject {
        put("format", "nightglass-personalization"); put("version", 1)
        put("name", name); put("face", face); put("accent", "%06X".format(accent))
        putJsonArray("complications") { complications.forEach { add(it) } }
        putJsonArray("deckOrder") { deckOrder.forEach { add(it) } }
        put("deckMask", deckMask); put("alwaysOn", alwaysOn); put("largeText", largeText); put("reduceMotion", reduceMotion)
    })
    companion object {
        val CAPABILITY: UUID = UUID.fromString("7a3b4005-6b6f-4f72-726f-772d6e696768")
        val faceNames = listOf("Classic", "Revenant", "Personal")
        val complicationNames = listOf("Battery", "Steps", "Weather", "Timer", "Alarm", "Notifications", "Distance", "Phone connection")
        val cardNames = listOf("Next up", "Move", "Weather", "Media", "Inbox")
        /** A validated, read-only query response also proves support when Android
         * still caches the characteristic list from pre-premium firmware. */
        fun discoveryResponse(frame: ByteArray): PremiumProfile? {
            if (frame.getOrNull(1) != 0x71.toByte()) return null
            val (nonce, profile) = parseFrame(frame) ?: return null
            return profile.takeIf { nonce == 0u }
        }
        fun parseFrame(frame: ByteArray): Pair<UInt, PremiumProfile>? = runCatching {
            require(frame.size == 48 && frame[0] == 1.toByte() && frame[1].toInt() in 0x70..0x71)
            fun byte(i: Int) = frame[i].toInt() and 255
            val length = byte(20); require(length in 1..24 && frame.drop(21 + length).all { it == 0.toByte() })
            val flags = byte(7); require(flags in 0..7)
            val p = PremiumProfile(String(frame, 21, length, Charsets.US_ASCII), byte(6),
                (byte(8) shl 16) or (byte(9) shl 8) or byte(10),
                (11..13).map(::byte), (14..18).map(::byte), byte(19),
                flags and 1 != 0, flags and 2 != 0, flags and 4 != 0).validate()
            ByteBuffer.wrap(frame, 2, 4).order(ByteOrder.LITTLE_ENDIAN).int.toUInt() to p
        }.getOrNull()
        fun fromJson(text: String): PremiumProfile {
            require(text.toByteArray().size <= 8192) { "Face pack exceeds 8 KB" }
            // This schema needs one object and flat arrays only. Bound nesting
            // before invoking a general JSON parser on a third-party file.
            var depth = 0; var quoted = false; var escaped = false
            text.forEach { c ->
                if (quoted) {
                    if (escaped) escaped = false else if (c == '\\') escaped = true else if (c == '"') quoted = false
                } else when (c) {
                    '"' -> quoted = true
                    '{', '[' -> { depth++; require(depth <= 4) { "Face pack is too deeply nested" } }
                    '}', ']' -> depth--
                }
            }
            val root = Json.parseToJsonElement(text).jsonObject
            val keys = setOf("format", "version", "name", "face", "accent", "complications", "deckOrder", "deckMask", "alwaysOn", "largeText", "reduceMotion")
            require(root.keys == keys) { "Unsupported or missing face-pack fields" }
            fun value(key: String) = root.getValue(key).jsonPrimitive
            require(value("format").content == "nightglass-personalization" && value("version").int == 1) { "Unsupported face-pack version" }
            val color = value("accent").content
            require(color.matches(Regex("[0-9a-fA-F]{6}"))) { "Accent must be a six-digit RGB color" }
            return PremiumProfile(value("name").content, value("face").int, color.toInt(16),
                root.getValue("complications").jsonArray.map { it.jsonPrimitive.int },
                root.getValue("deckOrder").jsonArray.map { it.jsonPrimitive.int }, value("deckMask").int,
                value("alwaysOn").boolean, value("largeText").boolean, value("reduceMotion").boolean).validate()
        }
    }
}
