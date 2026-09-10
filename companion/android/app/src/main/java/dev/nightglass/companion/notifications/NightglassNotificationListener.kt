package dev.nightglass.companion.notifications

import android.app.Notification
import android.app.RemoteInput
import android.content.Intent
import android.content.ComponentName
import android.media.MediaMetadata
import android.media.session.MediaSessionManager
import android.media.session.MediaController
import android.media.session.PlaybackState
import android.os.Bundle
import android.os.Build
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol
import dev.nightglass.companion.premium.*
import java.util.concurrent.ConcurrentHashMap
import java.security.SecureRandom

class NightglassNotificationListener : NotificationListenerService() {
    private var mediaController: MediaController? = null
    private val premium by lazy { PremiumPhoneContent(this, { mediaController }, { id ->
        keys[id]?.let { key -> activeNotifications.firstOrNull { it.key == key } }
    }) }
    private val mediaCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) { relayMedia() }
        override fun onPlaybackStateChanged(state: PlaybackState?) { relayMedia() }
        override fun onSessionDestroyed() { relayMedia() }
    }
    private val sessionsChanged = MediaSessionManager.OnActiveSessionsChangedListener {
        relayMedia()
    }
    companion object {
        private const val DISCORD_PACKAGE = "com.discord"
        private const val SPOTIFY_PACKAGE = "com.spotify.music"
        @Volatile private var current: NightglassNotificationListener? = null
        private val keys = ConcurrentHashMap<UInt, String>()
        private val random = SecureRandom()
        private data class ReplyRecord(val text: String, val status: Int)
        private val replyResults = object : LinkedHashMap<Long, ReplyRecord>(16, 0.75f, true) {
            override fun removeEldestEntry(eldest: MutableMap.MutableEntry<Long, ReplyRecord>?) =
                size > 16
        }
        fun syncCurrent() {
            val service = current ?: return
            keys.clear()
            synchronized(replyResults) { replyResults.clear() }
            NightglassConnectionService.send(service, NightglassProtocol.clear())
            service.activeNotifications.forEach { service.relay(it, false) }
            service.relayMedia()
        }
        fun syncMedia() { current?.relayMedia() }
        fun premiumDocument(request: PremiumRequest): PremiumDocument = current?.premium?.document(request)
            ?: PremiumDocument(request.kind, "Enable Nightglass notification access on your phone.")
        fun premiumAction(action: PremiumAction): Int = current?.premium?.perform(action) ?: 1
        fun clearPremium() { current?.premium?.clear() }
        fun seekMedia(deltaMs: Long): Boolean {
            val service = current ?: return false
            val controller = service.mediaController ?: return false
            val state = controller.playbackState ?: return false
            if ((state.actions and PlaybackState.ACTION_SEEK_TO) == 0L) return false
            val duration = controller.metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
            if (duration <= 0L) return false
            val target = (service.currentPosition(state)).plus(deltaMs).coerceIn(0L, duration)
            return runCatching {
                controller.transportControls.seekTo(target)
                service.relayMedia()
                true
            }.getOrDefault(false)
        }
        fun stopMedia(): Boolean {
            val service = current ?: return false
            val controller = service.mediaController ?: return false
            return runCatching {
                controller.transportControls.stop()
                service.relayMedia()
                true
            }.getOrDefault(false)
        }
        fun restartMedia(): Boolean {
            val service = current ?: return false
            val controller = service.mediaController ?: return false
            val state = controller.playbackState ?: return false
            if ((state.actions and PlaybackState.ACTION_SEEK_TO) == 0L) return false
            val resume = state.state == PlaybackState.STATE_PLAYING
            return runCatching {
                controller.transportControls.seekTo(0L)
                if (resume) controller.transportControls.play()
                service.relayMedia()
                true
            }.getOrDefault(false)
        }
        fun perform(id: UInt, dismiss: Boolean) {
            val service = current ?: return; val key = keys[id] ?: return
            if (dismiss) service.cancelNotification(key) else service.activeNotifications.firstOrNull { it.key == key }?.notification?.contentIntent?.let { runCatching { it.send() } }
        }
        fun reply(id: UInt, nonce: UInt, text: String): Int {
            val requestKey = (id.toLong() shl 32) xor nonce.toLong()
            synchronized(replyResults) {
                replyResults[requestKey]?.let { return if (it.text == text) it.status else 4 }
                val service = current ?: return 1
                val status = if (text.isBlank() || text.length > 96) 4 else {
                    val key = keys[id]
                    val notification = key?.let { notificationKey ->
                        service.activeNotifications.firstOrNull { it.key == notificationKey }
                            ?.notification
                    }
                    val actions = notification?.actions?.filter {
                        it.remoteInputs?.count { input -> input.allowFreeFormInput } == 1
                    }.orEmpty()
                    if (notification == null) 1
                    else if (actions.size != 1) 2
                    else {
                        val action = actions.single()
                        if (Build.VERSION.SDK_INT >= 31 && action.isAuthenticationRequired) 3
                        else {
                            val inputs = action.remoteInputs!!
                                .filter { it.allowFreeFormInput }.toTypedArray()
                            val results = Bundle().apply {
                                inputs.forEach { putCharSequence(it.resultKey, text) }
                            }
                            val intent = Intent()
                            RemoteInput.addResultsToIntent(inputs, intent, results)
                            runCatching { action.actionIntent.send(service, 0, intent); 0 }
                                .getOrDefault(5)
                        }
                    }
                }
                replyResults[requestKey] = ReplyRecord(text, status)
                return status
            }
        }
        private fun idForKey(key: String): UInt {
            keys.entries.firstOrNull { it.value == key }?.let { return it.key }
            var candidate: UInt
            do candidate = random.nextInt().toUInt() while (
                candidate == 0u || keys.putIfAbsent(candidate, key) != null)
            return candidate
        }
    }
    override fun onListenerConnected() {
        current = this
        runCatching { getSystemService(MediaSessionManager::class.java)
            .addOnActiveSessionsChangedListener(
                sessionsChanged,
                ComponentName(this, NightglassNotificationListener::class.java)) }
        syncCurrent()
    }
    override fun onListenerDisconnected() {
        premium.clear()
        runCatching { getSystemService(MediaSessionManager::class.java)
            .removeOnActiveSessionsChangedListener(sessionsChanged) }
        mediaController?.unregisterCallback(mediaCallback)
        mediaController = null
        if (current === this) current = null
    }
    override fun onNotificationPosted(sbn: StatusBarNotification) { relay(sbn, true); relayMedia() }
    override fun onNotificationRemoved(sbn: StatusBarNotification) { val id = keys.entries.firstOrNull { it.value == sbn.key }?.key ?: return; keys.remove(id); NightglassConnectionService.send(this, NightglassProtocol.remove(id)); relayMedia() }
    private fun relayMedia() {
        val manager = getSystemService(MediaSessionManager::class.java)
        val sessions = runCatching {
            manager.getActiveSessions(ComponentName(this, NightglassNotificationListener::class.java))
        }.getOrDefault(emptyList())
        // A dedicated Spotify surface should follow Spotify when it is active,
        // even if another player has a stale media session registered first.
        val controller = sessions.firstOrNull { it.packageName == SPOTIFY_PACKAGE }
            ?: sessions.firstOrNull {
            it.playbackState?.state == PlaybackState.STATE_PLAYING
        } ?: sessions.firstOrNull()
        if (mediaController?.sessionToken != controller?.sessionToken) {
            mediaController?.unregisterCallback(mediaCallback)
            mediaController = controller
            controller?.registerCallback(mediaCallback)
        }
        val metadata = controller?.metadata
        val title = metadata?.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty()
        val artist = metadata?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST).orEmpty()
        val state = controller?.playbackState
        val playing = state?.state == PlaybackState.STATE_PLAYING
        val duration = (metadata?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L)
            .coerceIn(0L, 604_800_000L)
        val position = if (state == null || duration == 0L) 0L
            else currentPosition(state).coerceIn(0L, duration)
        val seekable = state != null && duration > 0L &&
            (state.actions and PlaybackState.ACTION_SEEK_TO) != 0L
        NightglassConnectionService.send(
            this, NightglassProtocol.mediaState(title, artist, playing, controller != null,
                seekable, position, duration))
    }
    private fun currentPosition(state: PlaybackState): Long {
        if (state.state != PlaybackState.STATE_PLAYING || state.lastPositionUpdateTime <= 0L)
            return state.position.coerceAtLeast(0L)
        val elapsed = (SystemClock.elapsedRealtime() - state.lastPositionUpdateTime).coerceAtLeast(0L)
        return (state.position + elapsed * state.playbackSpeed).toLong().coerceAtLeast(0L)
    }
    private fun relay(sbn: StatusBarNotification, alert: Boolean) {
        if (sbn.packageName == packageName || sbn.isOngoing || (sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
        val id = idForKey(sbn.key)
        val e = sbn.notification.extras
        val app = if (sbn.packageName == DISCORD_PACKAGE) "Discord" else
            runCatching { packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString() }
                .getOrDefault(sbn.packageName)
        val rawTitle = e.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val rawBody = e.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
            ?.takeIf { it.isNotBlank() }
            ?: e.getCharSequence(Notification.EXTRA_TEXT)?.toString().orEmpty()
        val discord = if (sbn.packageName == DISCORD_PACKAGE) {
            val messages = e.getParcelableArray(Notification.EXTRA_MESSAGES)
                ?.mapNotNull { (it as? Bundle)?.getCharSequence(Notification.EXTRA_TEXT)?.toString() }
                ?.lastOrNull().orEmpty()
            val lines = e.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)
                ?.lastOrNull()?.toString().orEmpty()
            DiscordNotificationFormatter.format(
                rawTitle,
                e.getCharSequence(Notification.EXTRA_CONVERSATION_TITLE)?.toString().orEmpty(),
                e.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString().orEmpty(),
                rawBody,
                messages.ifBlank { lines })
        } else null
        val title = discord?.title ?: rawTitle
        val body = discord?.body ?: rawBody
        val category = when (sbn.notification.category) { Notification.CATEGORY_MESSAGE -> 1; Notification.CATEGORY_CALL -> 2; Notification.CATEGORY_EMAIL -> 3; Notification.CATEGORY_EVENT -> 4; Notification.CATEGORY_SOCIAL -> 5; else -> 0 }
        val replyable = sbn.notification.actions?.count {
            it.remoteInputs?.count { input -> input.allowFreeFormInput } == 1 &&
                (Build.VERSION.SDK_INT < 31 || !it.isAuthenticationRequired)
        } == 1
        NightglassConnectionService.send(this, NightglassProtocol.upsert(NightglassProtocol.RelayNotification(id, category, app, title, body, replyable), alert))
    }
}
