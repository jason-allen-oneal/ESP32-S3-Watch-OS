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
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol
import java.util.concurrent.ConcurrentHashMap
import java.security.SecureRandom

class NightglassNotificationListener : NotificationListenerService() {
    private var mediaController: MediaController? = null
    private val mediaCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) { relayMedia() }
        override fun onPlaybackStateChanged(state: PlaybackState?) { relayMedia() }
        override fun onSessionDestroyed() { relayMedia() }
    }
    private val sessionsChanged = MediaSessionManager.OnActiveSessionsChangedListener {
        relayMedia()
    }
    companion object {
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
        val controller = sessions.firstOrNull {
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
        val playing = controller?.playbackState?.state == PlaybackState.STATE_PLAYING
        NightglassConnectionService.send(
            this, NightglassProtocol.mediaState(title, artist, playing, controller != null))
    }
    private fun relay(sbn: StatusBarNotification, alert: Boolean) {
        if (sbn.packageName == packageName || sbn.isOngoing || (sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
        val id = idForKey(sbn.key)
        val e = sbn.notification.extras
        val app = runCatching { packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString() }.getOrDefault(sbn.packageName)
        val title = e.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val body = e.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString() ?: e.getCharSequence(Notification.EXTRA_TEXT)?.toString().orEmpty()
        val category = when (sbn.notification.category) { Notification.CATEGORY_MESSAGE -> 1; Notification.CATEGORY_CALL -> 2; Notification.CATEGORY_EMAIL -> 3; Notification.CATEGORY_EVENT -> 4; Notification.CATEGORY_SOCIAL -> 5; else -> 0 }
        val replyable = sbn.notification.actions?.count {
            it.remoteInputs?.count { input -> input.allowFreeFormInput } == 1 &&
                (Build.VERSION.SDK_INT < 31 || !it.isAuthenticationRequired)
        } == 1
        NightglassConnectionService.send(this, NightglassProtocol.upsert(NightglassProtocol.RelayNotification(id, category, app, title, body, replyable), alert))
    }
}
