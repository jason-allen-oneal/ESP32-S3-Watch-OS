package dev.nightglass.companion.update

/** Pure bounded policy for the physical watch-confirmation phase. */
internal class OtaConfirmationTracker(private val maximumPolls: Int = 12) {
    init { require(maximumPolls > 0) }

    var active: Boolean = false
        private set
    var polls: Int = 0
        private set

    fun begin() {
        if (!active) polls = 0
        active = true
    }

    fun reset() {
        active = false
        polls = 0
    }

    fun allowNextPoll(): Boolean {
        if (!active) return false
        polls += 1
        return polls <= maximumPolls
    }
}
