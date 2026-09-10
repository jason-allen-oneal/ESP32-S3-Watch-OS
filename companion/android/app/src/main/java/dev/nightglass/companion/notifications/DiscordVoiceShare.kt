package dev.nightglass.companion.notifications

import android.Manifest
import android.content.pm.PackageManager
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.FileProvider
import androidx.core.content.ContextCompat
import dev.nightglass.companion.protocol.NightglassProtocol
import dev.nightglass.companion.voice.VoiceAudioCodec
import java.io.File
import java.security.SecureRandom

/**
 * Hands one bounded watch recording to Discord's own share composer.
 *
 * The attachment is deliberately kept in the app cache and is never sent to
 * a Discord API. Discord chooses the destination and the user confirms Send.
 */
class DiscordVoiceShare(context: Context) {
    private val appContext = context.applicationContext
    private val expiryHandler = Handler(Looper.getMainLooper())
    private val random = SecureRandom()

    fun share(encoded: ByteArray): Boolean {
        if (encoded.isEmpty() || encoded.size > MAX_ENCODED_BYTES) return false
        val directory = File(appContext.cacheDir, SHARE_DIRECTORY)
        val output = runCatching {
            directory.mkdirs()
            purgeExpired(directory)
            uniqueFile(directory)
        }.getOrNull() ?: return false
        val wav = runCatching { VoiceAudioCodec.mulaw8kToWav(encoded) }.getOrNull()
            ?: run {
                output.delete()
                return false
            }
        return try {
            output.outputStream().use { stream -> stream.write(wav) }
            val uri = FileProvider.getUriForFile(
                appContext, "${appContext.packageName}.fileprovider", output)
            val intent = Intent(Intent.ACTION_SEND).apply {
                type = "audio/wav"
                putExtra(Intent.EXTRA_STREAM, uri)
                setPackage(DISCORD_PACKAGE)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
                clipData = ClipData.newRawUri("Nightglass voice note", uri)
            }
            if (intent.resolveActivity(appContext.packageManager) == null) {
                output.delete()
                return false
            }
            val launched = runCatching {
                appContext.startActivity(intent)
                true
            }.getOrDefault(false)
            if (!launched && !postShareNotification(intent)) {
                output.delete()
                return false
            }
            expiryHandler.postDelayed({ output.delete() }, SHARE_TTL_MS)
            true
        } catch (_: Throwable) {
            output.delete()
            false
        } finally {
            wav.fill(0)
        }
    }

    private fun purgeExpired(directory: File) {
        val cutoff = System.currentTimeMillis() - SHARE_TTL_MS
        directory.listFiles()?.forEach { file ->
            if (file.isFile && file.extension == "wav" && file.lastModified() < cutoff) {
                file.delete()
            }
        }
    }

    private fun postShareNotification(intent: Intent): Boolean {
        if (Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(
                appContext, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return false
        if (Build.VERSION.SDK_INT >= 26) {
            val manager = appContext.getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(NotificationChannel(
                NOTIFICATION_CHANNEL, "Discord voice replies", NotificationManager.IMPORTANCE_DEFAULT))
        }
        val pending = runCatching {
            PendingIntent.getActivity(
                appContext,
                NOTIFICATION_ID,
                intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
        }.getOrNull() ?: return false
        return runCatching {
            NotificationManagerCompat.from(appContext).notify(
                NOTIFICATION_ID,
                NotificationCompat.Builder(appContext, NOTIFICATION_CHANNEL)
                    .setSmallIcon(android.R.drawable.ic_btn_speak_now)
                    .setContentTitle("Discord voice note ready")
                    .setContentText("Tap to choose a conversation and confirm Send")
                    .setContentIntent(pending)
                    .setAutoCancel(true)
                    .setCategory(NotificationCompat.CATEGORY_MESSAGE)
                    .setPriority(NotificationCompat.PRIORITY_DEFAULT)
                    .build(),
            )
            true
        }.getOrDefault(false)
    }

    private fun uniqueFile(directory: File): File {
        repeat(8) {
            val candidate = File(directory, "voice-${random.nextLong().toULong().toString(16)}.wav")
            if (!candidate.exists()) return candidate
        }
        return File.createTempFile("voice-", ".wav", directory)
    }

    companion object {
        const val DISCORD_PACKAGE = "com.discord"
        const val SHARE_DIRECTORY = "nightglass-discord-share"
        const val SHARE_TTL_MS = 15 * 60 * 1_000L
        const val MAX_ENCODED_BYTES = NightglassProtocol.MAX_DISCORD_VOICE_REPLY_BYTES
        private const val NOTIFICATION_CHANNEL = "nightglass_discord_voice"
        private const val NOTIFICATION_ID = 0x4e47
    }
}
