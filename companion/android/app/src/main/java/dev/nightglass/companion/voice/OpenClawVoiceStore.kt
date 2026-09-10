package dev.nightglass.companion.voice

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Log
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import org.bouncycastle.crypto.generators.Ed25519KeyPairGenerator
import org.bouncycastle.crypto.params.Ed25519KeyGenerationParameters
import org.bouncycastle.crypto.params.Ed25519PrivateKeyParameters
import org.bouncycastle.crypto.params.Ed25519PublicKeyParameters
import org.bouncycastle.crypto.signers.Ed25519Signer
import org.bouncycastle.crypto.util.PrivateKeyFactory
import org.bouncycastle.crypto.util.PrivateKeyInfoFactory

data class OpenClawVoiceCredential(
    val url: String,
    val bootstrapToken: String?,
    val operatorToken: String?,
    val scopes: Set<String>,
    val tlsFingerprint: String?,
    val deviceId: String,
    val publicKeyRaw: ByteArray,
    val privateKeyPkcs8: ByteArray,
    val sessionKey: String?,
) {
    fun wipe() {
        publicKeyRaw.fill(0)
        privateKeyPkcs8.fill(0)
    }
}

class OpenClawVoiceStore(context: Context) {
    private val prefs = context.getSharedPreferences("nightglass_openclaw_voice", Context.MODE_PRIVATE)
    private val json = Json { ignoreUnknownKeys = true }

    fun provision(setup: OpenClawSetup) = synchronized(STORE_LOCK) {
        val existing = load()
        val identity = existing ?: generate(setup)
        val value = identity.copy(
            url = setup.url,
            bootstrapToken = setup.bootstrapToken,
            operatorToken = null,
            scopes = emptySet(),
            tlsFingerprint = setup.tlsFingerprint,
            sessionKey = null,
        )
        save(value)
        if (identity !== existing) identity.wipe()
        existing?.wipe()
        value.wipe()
    }

    fun persistOperatorToken(
        expected: OpenClawVoiceCredential,
        token: String,
        scopes: Collection<String>,
    ): Boolean = synchronized(STORE_LOCK) {
        require(token.length in 16..4096)
        val normalized = scopes.map { it.trim() }.filter { it.isNotEmpty() }.toSet()
        require(scopesAreAllowedHandoff(normalized))
        val current = load() ?: error("OpenClaw voice is not provisioned")
        if (!sameProvisioning(current, expected)) {
            current.wipe()
            return@synchronized false
        }
        val updated = current.copy(bootstrapToken = null, operatorToken = token, scopes = normalized)
        save(updated)
        current.wipe()
        updated.wipe()
        true
    }

    fun replaceSessionKey(
        expected: OpenClawVoiceCredential,
        expectedSessionKey: String?,
        sessionKey: String,
    ): Boolean = synchronized(STORE_LOCK) {
        require(isNightglassSessionKey(sessionKey))
        val current = load() ?: error("OpenClaw voice is not provisioned")
        if (!canReplaceSessionKey(current, expected, expectedSessionKey)) {
            current.wipe()
            return@synchronized false
        }
        val updated = current.copy(sessionKey = sessionKey)
        save(updated)
        current.wipe()
        updated.wipe()
        true
    }

    fun load(): OpenClawVoiceCredential? = synchronized(STORE_LOCK) {
        val packed = prefs.getString(KEY, null) ?: return null
        val clear = try {
            decrypt(Base64.getDecoder().decode(packed))
        } catch (error: Throwable) {
            Log.w("NightglassLink", "OpenClaw credential decrypt failed: ${error.javaClass.simpleName}")
            return null
        }
        try {
            val root = json.parseToJsonElement(clear.toString(Charsets.UTF_8)).jsonObject
            val publicKey = Base64.getDecoder().decode(root.getValue("publicKey").jsonPrimitive.content)
            val privateKey = Base64.getDecoder().decode(root.getValue("privateKey").jsonPrimitive.content)
            val deviceId = root.getValue("deviceId").jsonPrimitive.content
            val sessionKey = root["sessionKey"]?.jsonPrimitive?.content.orEmpty()
            require(publicKey.size == 32 && privateKey.size in 32..128 && deviceId == sha256(publicKey))
            require(sessionKey.isEmpty() || isNightglassSessionKey(sessionKey))
            OpenClawVoiceCredential(
                url = root.getValue("url").jsonPrimitive.content,
                bootstrapToken = root["bootstrapToken"]?.jsonPrimitive?.content?.takeIf { it.isNotEmpty() },
                operatorToken = root["operatorToken"]?.jsonPrimitive?.content?.takeIf { it.isNotEmpty() },
                scopes = root["scopes"]?.jsonPrimitive?.content?.split(',')?.filter { it.isNotEmpty() }?.toSet().orEmpty(),
                tlsFingerprint = root["tlsFingerprint"]?.jsonPrimitive?.content?.takeIf { it.isNotEmpty() },
                deviceId = deviceId,
                publicKeyRaw = publicKey,
                privateKeyPkcs8 = privateKey,
                sessionKey = sessionKey.takeIf { it.isNotEmpty() },
            )
        } catch (error: Throwable) {
            Log.w("NightglassLink", "OpenClaw credential parse failed: ${error.javaClass.simpleName}")
            null
        } finally {
            clear.fill(0)
        }
    }

    fun clear() = synchronized(STORE_LOCK) { prefs.edit().remove(KEY).commit() }

    fun sign(payload: String, credential: OpenClawVoiceCredential): String {
        val privateKey = PrivateKeyFactory.createKey(credential.privateKeyPkcs8) as Ed25519PrivateKeyParameters
        val signer = Ed25519Signer()
        signer.init(true, privateKey)
        val bytes = payload.toByteArray(Charsets.UTF_8)
        signer.update(bytes, 0, bytes.size)
        bytes.fill(0)
        return Base64.getUrlEncoder().withoutPadding().encodeToString(signer.generateSignature())
    }

    private fun generate(setup: OpenClawSetup): OpenClawVoiceCredential {
        val generator = Ed25519KeyPairGenerator()
        generator.init(Ed25519KeyGenerationParameters(SecureRandom()))
        val pair = generator.generateKeyPair()
        val publicKey = (pair.public as Ed25519PublicKeyParameters).encoded
        val privateKey = PrivateKeyInfoFactory.createPrivateKeyInfo(
            pair.private as Ed25519PrivateKeyParameters).encoded
        return OpenClawVoiceCredential(setup.url, setup.bootstrapToken, null, emptySet(),
            setup.tlsFingerprint, sha256(publicKey), publicKey, privateKey, null)
    }

    private fun save(value: OpenClawVoiceCredential) {
        val clear = buildJsonObject {
            put("url", value.url)
            put("bootstrapToken", value.bootstrapToken.orEmpty())
            put("operatorToken", value.operatorToken.orEmpty())
            put("scopes", value.scopes.sorted().joinToString(","))
            put("tlsFingerprint", value.tlsFingerprint.orEmpty())
            put("deviceId", value.deviceId)
            put("publicKey", Base64.getEncoder().encodeToString(value.publicKeyRaw))
            put("privateKey", Base64.getEncoder().encodeToString(value.privateKeyPkcs8))
            put("sessionKey", value.sessionKey.orEmpty())
        }.toString().toByteArray(Charsets.UTF_8)
        val encrypted = encrypt(clear)
        clear.fill(0)
        check(prefs.edit().putString(KEY, Base64.getEncoder().encodeToString(encrypted)).commit())
        encrypted.fill(0)
    }

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
            init(KeyGenParameterSpec.Builder(ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256).build())
            generateKey()
        }
    }

    private fun encrypt(clear: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        return cipher.iv + cipher.doFinal(clear)
    }

    private fun decrypt(packed: ByteArray): ByteArray {
        require(packed.size in 29..16384)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, packed.copyOfRange(0, 12)))
        return cipher.doFinal(packed, 12, packed.size - 12)
    }

    companion object {
        // Stock --voice-node pairing starts at read+talk. The companion then
        // uses OpenClaw's approval-bound live upgrade to add operator.write;
        // the Gateway requires upgrades to retain existing scopes and also
        // normalizes write to include read. Keep both profiles exact so admin,
        // approvals, pairing, questions, and secret scopes can never slip in.
        val BOOTSTRAP_SCOPES = setOf("operator.read", "operator.talk")
        val REQUIRED_SCOPES = setOf("operator.read", "operator.talk", "operator.write")
        const val SESSION_KEY_PREFIX = "agent:main:dashboard:"
        fun isNightglassSessionKey(key: String): Boolean {
            if (!key.startsWith(SESSION_KEY_PREFIX)) return false
            val suffix = key.removePrefix(SESSION_KEY_PREFIX)
            return runCatching { UUID.fromString(suffix).toString() == suffix }.getOrDefault(false)
        }
        fun scopesAreExactlyRequired(scopes: Collection<String>): Boolean =
            scopes.map { it.trim() }.filter { it.isNotEmpty() }.toSet() == REQUIRED_SCOPES
        fun scopesAreAllowedHandoff(scopes: Collection<String>): Boolean {
            val normalized = scopes.map { it.trim() }.filter { it.isNotEmpty() }.toSet()
            return normalized == BOOTSTRAP_SCOPES || normalized == REQUIRED_SCOPES
        }
        internal fun sameProvisioning(
            current: OpenClawVoiceCredential,
            expected: OpenClawVoiceCredential,
        ): Boolean =
            current.url == expected.url &&
                current.bootstrapToken == expected.bootstrapToken &&
                current.operatorToken == expected.operatorToken &&
                current.scopes == expected.scopes &&
                current.tlsFingerprint == expected.tlsFingerprint &&
                current.deviceId == expected.deviceId &&
                current.publicKeyRaw.contentEquals(expected.publicKeyRaw) &&
                current.sessionKey == expected.sessionKey

        internal fun canReplaceSessionKey(
            current: OpenClawVoiceCredential,
            expected: OpenClawVoiceCredential,
            expectedSessionKey: String?,
        ): Boolean = sameProvisioning(current, expected) &&
            current.sessionKey == expectedSessionKey

        private val STORE_LOCK = Any()
        private const val KEY = "credential"
        private const val ALIAS = "nightglass-openclaw-voice-v1"
        private fun sha256(bytes: ByteArray) = MessageDigest.getInstance("SHA-256")
            .digest(bytes).joinToString("") { "%02x".format(it) }
    }
}
