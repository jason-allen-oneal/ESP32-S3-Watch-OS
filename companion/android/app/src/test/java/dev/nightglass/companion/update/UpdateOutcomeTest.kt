package dev.nightglass.companion.update

import org.junit.Assert.assertEquals
import org.junit.Test

class UpdateOutcomeTest {
    @Test fun validationOrReconnectAloneCannotBecomeInstalled() {
        assertEquals(UpdateOutcome.Confirmation.IGNORE, UpdateOutcome.confirm(null, "0.2.26", false, 0))
        assertEquals(UpdateOutcome.Confirmation.IGNORE, UpdateOutcome.confirm("0.2.26", "0.2.26", false, 4))
        assertEquals(UpdateOutcome.Confirmation.PENDING_HEALTH, UpdateOutcome.confirm("0.2.26", "0.2.26", true, 0))
    }
    @Test fun verifiedTargetAndRollbackHaveDifferentOutcomes() {
        assertEquals(UpdateOutcome.Confirmation.INSTALLED, UpdateOutcome.confirm("0.2.26", "0.2.26", false, 0))
        assertEquals(UpdateOutcome.Confirmation.DIFFERENT_VERSION, UpdateOutcome.confirm("0.2.26", "0.2.25", false, 0))
    }
}
