package dev.nightglass.companion.ble

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat

/**
 * Restores the already-authorized companion service after normal Android
 * lifecycle events. It deliberately requires an existing pinned address: a
 * reboot or package update must never create a new pairing or scan for an
 * unknown watch on the user's behalf.
 */
class NightglassLifecycleReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        val action = intent?.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_MY_PACKAGE_REPLACED) return

        val hasPinnedWatch = context.getSharedPreferences(
            PREFS, Context.MODE_PRIVATE).getString(PINNED_ADDRESS, null)
            ?.isNotBlank() == true
        if (!hasPinnedWatch) return

        runCatching {
            ContextCompat.startForegroundService(
                context,
                Intent(context, NightglassConnectionService::class.java)
                    .setAction(NightglassConnectionService.ACTION_CONNECT),
            )
        }
    }

    private companion object {
        const val PREFS = "nightglass_link"
        const val PINNED_ADDRESS = "pinned_address"
    }
}
