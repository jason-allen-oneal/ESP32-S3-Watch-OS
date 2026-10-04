package dev.nightglass.companion.update

/** Transport validation is not installation: require post-reboot identity and health. */
object UpdateOutcome {
    enum class Confirmation { IGNORE, PENDING_HEALTH, INSTALLED, DIFFERENT_VERSION }
    fun confirm(expected: String?, running: String, pendingHealth: Boolean, result: Int): Confirmation =
        when {
            expected == null || result != 0 -> Confirmation.IGNORE
            running != expected -> Confirmation.DIFFERENT_VERSION
            pendingHealth -> Confirmation.PENDING_HEALTH
            else -> Confirmation.INSTALLED
        }
}
