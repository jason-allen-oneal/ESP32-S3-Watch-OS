package dev.nightglass.companion.voice

import org.junit.Assert.*
import org.junit.Test

class OpenClawHealthPolicyTest {
    @Test fun healthPolicyRequiresTwoTransientFailures() {
        val unavailable = OpenClawProbeResult(true, false, false)
        val first = OpenClawHealthPolicy.decide(unavailable, true, 0)
        assertEquals(OpenClawHealth.DEGRADED, first.state)
        assertEquals(1, first.consecutiveFailures)
        val second = OpenClawHealthPolicy.decide(unavailable, true, first.consecutiveFailures)
        assertEquals(OpenClawHealth.UNAVAILABLE, second.state)
        assertEquals(2, second.consecutiveFailures)
    }

    @Test fun fatalFailsRedAndSuccessRecoversGreen() {
        val fatal = OpenClawHealthPolicy.decide(
            OpenClawProbeResult(true, false, true), true, 0)
        assertEquals(OpenClawHealth.UNAVAILABLE, fatal.state)
        val recovered = OpenClawHealthPolicy.decide(
            OpenClawProbeResult(true, true, false), true, fatal.consecutiveFailures)
        assertEquals(OpenClawHealth.HEALTHY, recovered.state)
        assertEquals(0, recovered.consecutiveFailures)
    }

    @Test fun incompleteSetupOrPhoneNetworkIsYellow() {
        assertEquals(OpenClawHealth.DEGRADED, OpenClawHealthPolicy.decide(
            OpenClawProbeResult(false, false, false), true, 2).state)
        assertEquals(OpenClawHealth.DEGRADED, OpenClawHealthPolicy.decide(
            OpenClawProbeResult(true, false, false), false, 2).state)
    }
}
