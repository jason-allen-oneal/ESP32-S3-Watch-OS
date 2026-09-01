package dev.nightglass.companion.update

import android.content.ContentResolver
import android.database.Cursor
import android.net.Uri
import android.provider.OpenableColumns
import dev.nightglass.companion.protocol.NightglassProtocol
import org.json.JSONObject
import java.security.MessageDigest
import java.security.SecureRandom

data class PreparedOtaPackage(
    val firmware: Uri,
    val manifest: NightglassProtocol.OtaManifest,
    val session: ULong,
)

object OtaPackageLoader {
    private const val ALGORITHM = "ecdsa-p256-sha256-der"
    private const val BOARD = "nightglass-waveshare-esp32s3r8-2.06-v1.0"
    private const val PARTITION = "nightglass-32m-r1"

    fun load(resolver: ContentResolver, uris: List<Uri>): PreparedOtaPackage {
        require(uris.size == 4) {
            "Select firmware.bin, manifest.json, manifest.payload, and manifest.sig"
        }
        val named = uris.associateBy { displayName(resolver, it) }
        require(named.size == 4) { "Package filenames must be unique" }
        val firmware = requireNotNull(named["firmware.bin"]) { "firmware.bin is missing" }
        val manifestUri = requireNotNull(named["manifest.json"]) { "manifest.json is missing" }
        val signatureUri = requireNotNull(named["manifest.sig"]) { "manifest.sig is missing" }
        val payloadUri = requireNotNull(named["manifest.payload"]) { "manifest.payload is missing" }
        val jsonBytes = resolver.openInputStream(manifestUri)!!.use { input ->
            readBounded(input, 8192).also { require(it.size >= 2) { "Manifest is empty" } }
        }
        val json = JSONObject(jsonBytes.toString(Charsets.UTF_8))
        val signatureObject = json.getJSONObject("signature")
        require(signatureObject.getString("algorithm") == ALGORITHM &&
            signatureObject.getString("file") == "manifest.sig") {
            "Package signature algorithm is unsupported"
        }
        val signature = resolver.openInputStream(signatureUri)!!.use { readBounded(it, 72) }
        require(validDerEnvelope(signature)) { "Manifest signature is not bounded DER" }
        val expectedDigest = parseHex(json.getString("image_sha256"))
        require(expectedDigest.size == 32) { "Image digest must be SHA-256" }
        val digest = MessageDigest.getInstance("SHA-256")
        var imageSize = 0L
        resolver.openInputStream(firmware)!!.use { input ->
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                require(count != 0) { "Firmware stream stalled" }
                imageSize += count
                require(imageSize <= 6L * 1024 * 1024) { "Firmware exceeds OTA slot" }
                digest.update(buffer, 0, count)
            }
        }
        require(imageSize > 0 && imageSize == json.getLong("image_size")) {
            "Firmware size does not match manifest"
        }
        require(MessageDigest.isEqual(expectedDigest, digest.digest())) {
            "Firmware digest does not match manifest"
        }
        val board = json.getString("board_id")
        val partition = json.getString("partition_id")
        require(board == BOARD && partition == PARTITION) { "Package targets another watch" }
        val formatVersion = json.getLong("format_version")
        val partitionRevision = json.getLong("partition_revision")
        val secureVersion = json.getLong("secure_version")
        val appVersion = json.getString("app_version")
        require(formatVersion == 1L && partitionRevision == 1L) {
            "Package manifest revision is unsupported"
        }
        require(secureVersion in 0..UInt.MAX_VALUE.toLong()) {
            "Package secure version is invalid"
        }
        require(canonicalText(appVersion, 31)) { "Package version is not canonical" }
        val canonicalPayload = (
            "NIGHTGLASS-OTA-MANIFEST\n" +
                "format=$formatVersion\n" +
                "board=$board\n" +
                "partition=$partition\n" +
                "partition_revision=$partitionRevision\n" +
                "version=$appVersion\n" +
                "secure_version=$secureVersion\n" +
                "size=$imageSize\n" +
                "sha256=${json.getString("image_sha256")}\n"
            ).toByteArray(Charsets.US_ASCII)
        val suppliedPayload = resolver.openInputStream(payloadUri)!!.use { readBounded(it, 512) }
        require(MessageDigest.isEqual(canonicalPayload, suppliedPayload)) {
            "Manifest payload does not match canonical metadata"
        }
        val manifest = NightglassProtocol.OtaManifest(
            formatVersion, board, partition, partitionRevision, appVersion,
            secureVersion, imageSize, expectedDigest, signature)
        // A fresh unpredictable transfer nonce is still bound to this package's digest.
        var digestBinding = 0uL
        for (index in 0 until 8) {
            digestBinding = digestBinding or ((expectedDigest[index].toInt() and 0xff).toULong() shl
                (index * 8))
        }
        var session = SecureRandom().nextLong().toULong() xor digestBinding xor imageSize.toULong()
        if (session == 0uL) session = 1uL
        return PreparedOtaPackage(firmware, manifest, session)
    }

    private fun displayName(resolver: ContentResolver, uri: Uri): String {
        val cursor: Cursor = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME),
            null, null, null) ?: error("Package file metadata unavailable")
        cursor.use {
            require(it.moveToFirst()) { "Package file metadata unavailable" }
            return it.getString(0)
        }
    }

    private fun parseHex(value: String): ByteArray {
        require(value.length == 64 && value.all { it in "0123456789abcdef" }) {
            "Image digest is not canonical lowercase hex"
        }
        return ByteArray(32) { index -> value.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
    }

    private fun validDerEnvelope(signature: ByteArray): Boolean {
        if (signature.size !in 8..72 || signature[0] != 0x30.toByte()) return false
        val declared = signature[1].toInt() and 0xff
        return declared < 0x80 && declared == signature.size - 2
    }

    private fun canonicalText(value: String, maximum: Int): Boolean {
        val bytes = value.toByteArray(Charsets.US_ASCII)
        return bytes.size in 1..maximum && bytes.all {
            (it.toInt() and 0xff) in 0x21..0x7e && it != '\\'.code.toByte() &&
                it != '='.code.toByte()
        }
    }

    private fun readBounded(input: java.io.InputStream, maximum: Int): ByteArray {
        val output = java.io.ByteArrayOutputStream(minOf(maximum, 1024))
        val buffer = ByteArray(1024)
        var total = 0
        while (true) {
            val count = input.read(buffer, 0, minOf(buffer.size, maximum + 1 - total))
            if (count < 0) break
            require(count != 0) { "Package stream stalled" }
            total += count
            require(total <= maximum) { "Package file exceeds size limit" }
            output.write(buffer, 0, count)
        }
        return output.toByteArray()
    }
}
