package dev.nightglass.companion.premium

import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.PlaybackState
import android.media.VolumeProvider
import android.os.Build
import android.os.SystemClock
import android.provider.Settings
import android.service.notification.StatusBarNotification
import androidx.core.app.NotificationCompat

/** Uses only the selected Android media session and live notification PendingIntents.
 * No provider passwords/tokens, URL fetching, persisted message history or self-bot APIs. */
class PremiumPhoneContent(
    private val context: Context,
    private val controller: () -> MediaController?,
    private val notification: (UInt) -> StatusBarNotification?,
) {
    private val fence = PremiumActionFence()
    private val operations = HashMap<Pair<Int, Int>, () -> Int>()
    private var lastRequest: PremiumRequest? = null
    @Synchronized fun clear() { fence.clear(); operations.clear(); lastRequest = null }
    @Synchronized fun document(request: PremiumRequest): PremiumDocument {
        require(lastRequest != request) { "Duplicate content request" }
        clear()
        lastRequest = request
        val document = if (request.kind == 1) media() else conversation(request.target)
        fence.bind(request, document.actions, SystemClock.elapsedRealtime())
        return document
    }
    @Synchronized fun perform(action: PremiumAction): Int {
        if (!fence.accept(action, SystemClock.elapsedRealtime())) return 4
        return runCatching { operations[action.action to action.index]?.invoke() ?: 2 }.getOrDefault(5)
    }
    private fun media(): PremiumDocument {
        val player = controller() ?: return PremiumDocument(1, "No active player. Start Spotify or another media app on your phone.")
        val metadata = player.metadata
        val session = player.sessionToken
        val identity = metadata?.getString(MediaMetadata.METADATA_KEY_MEDIA_ID)
            ?: metadata?.getString(MediaMetadata.METADATA_KEY_TITLE)
        fun sameSession() = controller()?.let {
            it.sessionToken == session && (it.metadata?.getString(MediaMetadata.METADATA_KEY_MEDIA_ID)
                ?: it.metadata?.getString(MediaMetadata.METADATA_KEY_TITLE)) == identity
        } == true
        val offered = ArrayList<ContentAction>()
        val queue = player.queue.orEmpty().take(4)
        val canQueue = (player.playbackState?.actions ?: 0L) and PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM != 0L
        if (canQueue) queue.forEachIndexed { index, item ->
            offered += ContentAction(1, index, "Play ${index + 1}: ${item.description.title ?: "Queue item"}")
            operations[1 to index] = {
                // Queue IDs and ordering can change while the watch is displaying them.
                val current = controller()
                if (!sameSession() || current?.queue.orEmpty().none {
                    it.queueId == item.queueId && it.description.mediaId == item.description.mediaId &&
                        it.description.title?.toString() == item.description.title?.toString()
                } || (current?.playbackState?.actions ?: 0L) and PlaybackState.ACTION_SKIP_TO_QUEUE_ITEM == 0L) 4
                else { current!!.transportControls.skipToQueueItem(item.queueId); 0 }
            }
        }
        val output = player.playbackInfo
        val adjustable = output != null && output.volumeControl != VolumeProvider.VOLUME_CONTROL_FIXED
        // Four queue entries, real session volume, and an explicit phone handoff.
        if (adjustable) listOf(2 to AudioManager.ADJUST_LOWER, 3 to AudioManager.ADJUST_RAISE).forEach { (id, direction) ->
            offered += ContentAction(id, 0, if (direction < 0) "Output volume -" else "Output volume +")
            operations[id to 0] = {
                if (!sameSession() || controller()?.playbackInfo?.volumeControl == VolumeProvider.VOLUME_CONTROL_FIXED) 4
                else { controller()!!.adjustVolume(direction, AudioManager.FLAG_SHOW_UI); 0 }
            }
        }
        player.sessionActivity?.let { pending ->
            offered += ContentAction(4, 0, "Choose output: open player on phone")
            operations[4 to 0] = {
                if (!sameSession() || controller()?.sessionActivity != pending) 4 else send(pending)
            }
        }
        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty().take(72).ifBlank { "Active player" }
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST).orEmpty().take(48)
        val remote = output?.playbackType == MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE
        val details = buildString {
            append(title); if (artist.isNotBlank()) append("\n$artist")
            append("\n\nOutput: ${if (remote) "app-managed remote device" else "phone audio route"}")
            if (output != null) append("\nVolume: ${output.currentVolume}/${output.maxVolume}${if (!adjustable) " (fixed)" else ""}")
            append("\nChange devices in the phone player's output picker.")
            append("\n\nQueue")
            if (queue.isEmpty()) append(" not exposed by this app.")
            else queue.forEachIndexed { i, item -> append("\n${i + 1}. ${item.description.title?.toString()?.take(48) ?: "Untitled"}") }
            if (queue.isNotEmpty() && !canQueue) append("\nThis app does not allow queue selection.")
        }
        val bitmap = metadata?.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata?.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
        return PremiumDocument(1, details, offered.take(7), bitmap?.let(::artwork))
    }
    private fun artwork(source: Bitmap): ByteArray? = runCatching {
        val small = Bitmap.createScaledBitmap(source, 40, 40, true)
        try {
            val pixels = IntArray(1600); small.getPixels(pixels, 0, 40, 0, 0, 40, 40)
            ByteArray(3200).also { out -> pixels.forEachIndexed { index, color ->
                val value = ((color shr 19) and 31 shl 11) or
                    ((color shr 10) and 63 shl 5) or ((color shr 3) and 31)
                out[index * 2] = value.toByte(); out[index * 2 + 1] = (value shr 8).toByte()
            } }
        } finally { if (small !== source) small.recycle() }
    }.getOrNull()
    @Suppress("DEPRECATION")
    private fun conversation(id: UInt): PremiumDocument {
        val source = notification(id) ?: return PremiumDocument(2, "This notification is no longer available.")
        val notice = source.notification
        val extras = notice.extras
        val title = extras.getCharSequence(Notification.EXTRA_CONVERSATION_TITLE)?.toString()
            ?: extras.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val style = NotificationCompat.MessagingStyle.extractMessagingStyleFromNotification(notice)
        val messages = (style?.historicMessages.orEmpty() + style?.messages.orEmpty())
            .distinctBy { Triple(it.timestamp, it.sender?.toString(), it.text?.toString()) }.takeLast(8)
        val text = buildString {
            append(title.take(100)); append("\n\n")
            if (messages.isEmpty()) append((extras.getCharSequence(Notification.EXTRA_BIG_TEXT)
                ?: extras.getCharSequence(Notification.EXTRA_TEXT))?.toString().orEmpty().take(1200))
            else messages.forEach { message ->
                val sender = message.person?.name ?: message.sender
                if (!sender.isNullOrBlank()) append("${sender.toString().take(48)}: ")
                append(message.text?.toString().orEmpty().take(120))
                if (message.dataUri != null) append(" [Attachment: ${message.dataMimeType.orEmpty().take(40)} - open on phone]")
                append("\n\n")
            }
            append("Notification history only. For older messages, attachments and additional reactions, open the conversation on your phone.")
        }
        val offered = ArrayList<ContentAction>()
        fun stillCurrent(): StatusBarNotification? = notification(id)?.takeIf {
            it.key == source.key && it.postTime == source.postTime
        }
        notice.actions.orEmpty().filter { action ->
            action.remoteInputs.isNullOrEmpty() && action.actionIntent != null &&
                (Build.VERSION.SDK_INT < 31 || !action.isAuthenticationRequired)
        }.mapNotNull { action ->
            val caption = PremiumContent.actionCaption(action.title?.toString().orEmpty())
            // Never display an unlabeled/destructive unknown glyph as a generic button.
            caption.takeIf { it.isNotBlank() }?.let { action to it }
        }.take(4).forEachIndexed { index, (action, caption) ->
            offered += ContentAction(5, index, caption)
            val pending = action.actionIntent
            operations[5 to index] = {
                val live = stillCurrent()?.notification?.actions.orEmpty().firstOrNull { it.actionIntent == pending }
                if (live == null || (Build.VERSION.SDK_INT >= 31 && live.isAuthenticationRequired) || !live.remoteInputs.isNullOrEmpty()) 4
                else send(pending)
            }
        }
        notice.contentIntent?.let { pending ->
            offered += ContentAction(6, 0, "Open conversation / attachments")
            operations[6 to 0] = { if (stillCurrent()?.notification?.contentIntent != pending) 4 else send(pending) }
        }
        return PremiumDocument(2, text, offered)
    }
    private fun send(pending: PendingIntent): Int = try { pending.send(); 0 }
        catch (_: PendingIntent.CanceledException) { 1 }
}
