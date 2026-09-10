package dev.nightglass.companion.premium

import android.content.Context
import dev.nightglass.companion.ble.NightglassConnectionService
import java.security.SecureRandom

object PremiumStore {
    @Volatile var available = false
        private set
    @Volatile var status = "Connect a premium-capable watch to apply changes."
        private set
    @Volatile private var pendingNonce = 0u
    @Volatile private var pendingAt = 0L
    private fun prefs(context: Context) = context.getSharedPreferences("nightglass_premium", Context.MODE_PRIVATE)
    fun draft(context: Context): PremiumProfile = runCatching {
        PremiumProfile.fromJson(prefs(context).getString("draft", null) ?: return PremiumProfile())
    }.getOrDefault(PremiumProfile())
    fun confirmed(context: Context): PremiumProfile? = runCatching {
        PremiumProfile.fromJson(prefs(context).getString("confirmed", null) ?: return null)
    }.getOrNull()
    fun saveDraft(context: Context, profile: PremiumProfile) {
        prefs(context).edit().putString("draft", profile.validate().json()).apply()
    }
    @Synchronized fun linked(supported: Boolean) {
        available = supported; pendingNonce = 0u
        status = if (supported) "Connected. Changes apply only when you choose Apply."
            else "This firmware does not support premium editing. Your phone draft is safe."
    }
    @Synchronized fun disconnected() {
        available = false; pendingNonce = 0u
        status = "Watch disconnected. Draft saved on phone; changes are not applied."
    }
    @Synchronized fun failed() { pendingNonce = 0u; status = "Watch did not save the changes. Your phone draft is safe." }
    @Synchronized fun apply(context: Context, profile: PremiumProfile): Boolean {
        saveDraft(context, profile)
        if (!available) { status = "Saved on phone. Connect premium firmware to apply."; return false }
        pendingNonce = SecureRandom().nextInt().toUInt().let { if (it == 0u) 1u else it }
        pendingAt = android.os.SystemClock.elapsedRealtime()
        status = "Applying to watch..."
        if (!NightglassConnectionService.send(context, profile.frame(pendingNonce))) { failed(); return false }
        return true
    }
    @Synchronized fun receive(context: Context, frame: ByteArray): Boolean {
        if (frame.getOrNull(1) != 0x71.toByte()) return false
        val (nonce, profile) = PremiumProfile.parseFrame(frame) ?: return false
        if (!available) return false
        prefs(context).edit().putString("confirmed", profile.json()).apply()
        if (nonce != 0u && nonce == pendingNonce) { pendingNonce = 0u; status = "Saved on watch." }
        return true
    }
    @Synchronized fun currentStatus(): String {
        if (pendingNonce != 0u && android.os.SystemClock.elapsedRealtime() - pendingAt > 10_000) {
            pendingNonce = 0u; status = "No save confirmation. Draft kept on phone; reconnect and check the watch."
        }
        return status
    }
}
