package dev.nightglass.companion.notifications

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol
import java.util.concurrent.ConcurrentHashMap

class NightglassNotificationListener : NotificationListenerService() {
    companion object {
        @Volatile private var current: NightglassNotificationListener? = null
        private val keys = ConcurrentHashMap<UInt, String>()
        fun perform(id: UInt, dismiss: Boolean) {
            val service = current ?: return; val key = keys[id] ?: return
            if (dismiss) service.cancelNotification(key) else service.activeNotifications.firstOrNull { it.key == key }?.notification?.contentIntent?.let { runCatching { it.send() } }
        }
        private fun stableId(key: String): UInt { val value = key.hashCode().toUInt(); return if (value == 0u) 1u else value }
    }
    override fun onListenerConnected() { current = this; activeNotifications.forEach(::relay) }
    override fun onListenerDisconnected() { if (current === this) current = null }
    override fun onNotificationPosted(sbn: StatusBarNotification) { relay(sbn) }
    override fun onNotificationRemoved(sbn: StatusBarNotification) { val id = stableId(sbn.key); keys.remove(id); NightglassConnectionService.send(this, NightglassProtocol.remove(id)) }
    private fun relay(sbn: StatusBarNotification) {
        if (sbn.packageName == packageName || sbn.isOngoing || (sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
        val id = stableId(sbn.key); keys[id] = sbn.key
        val e = sbn.notification.extras
        val app = runCatching { packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString() }.getOrDefault(sbn.packageName)
        val title = e.getCharSequence(Notification.EXTRA_TITLE)?.toString().orEmpty()
        val body = e.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString() ?: e.getCharSequence(Notification.EXTRA_TEXT)?.toString().orEmpty()
        val category = when (sbn.notification.category) { Notification.CATEGORY_MESSAGE -> 1; Notification.CATEGORY_CALL -> 2; Notification.CATEGORY_EMAIL -> 3; Notification.CATEGORY_EVENT -> 4; Notification.CATEGORY_SOCIAL -> 5; else -> 0 }
        NightglassConnectionService.send(this, NightglassProtocol.upsert(NightglassProtocol.RelayNotification(id, category, app, title, body)))
    }
}
