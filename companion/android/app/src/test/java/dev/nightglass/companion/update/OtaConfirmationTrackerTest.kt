package dev.nightglass.companion.update

import org.junit.Assert.*
import org.junit.Test

class OtaConfirmationTrackerTest {
    @Test fun lostStatusNotificationsRemainBounded() {
        val tracker = OtaConfirmationTracker(3)
        assertFalse(tracker.allowNextPoll())
        tracker.begin()
        assertTrue(tracker.allowNextPoll())
        assertTrue(tracker.allowNextPoll())
        assertTrue(tracker.allowNextPoll())
        assertFalse(tracker.allowNextPoll())
        assertEquals(4, tracker.polls)
    }

    @Test fun repeatedConfirmationStatusDoesNotResetDeadline() {
        val tracker = OtaConfirmationTracker(2)
        tracker.begin()
        assertTrue(tracker.allowNextPoll())
        tracker.begin()
        assertEquals(1, tracker.polls)
        assertTrue(tracker.allowNextPoll())
        assertFalse(tracker.allowNextPoll())
        tracker.reset()
        assertFalse(tracker.active)
        assertEquals(0, tracker.polls)
    }
}
