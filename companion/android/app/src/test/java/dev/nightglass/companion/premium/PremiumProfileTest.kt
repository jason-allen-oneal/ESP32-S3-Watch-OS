package dev.nightglass.companion.premium

import org.junit.Assert.*
import org.junit.Test

class PremiumProfileTest {
    @Test fun matchesFirmwareGoldenVector() {
        val frame = PremiumProfile().frame(0x12345678u)
        assertEquals("0170785634120100a8ff3201020000010203041f0d4d79204e69676874676c6173730000000000000000000000000000",
            frame.joinToString("") { "%02x".format(it) })
        assertEquals(0x12345678u to PremiumProfile(), PremiumProfile.parseFrame(frame))
    }
    @Test fun readOnlyResponseProvesSupportWithStaleCharacteristicCache() {
        val profile = PremiumProfile(face = 2, reduceMotion = true)
        assertEquals(profile, PremiumProfile.discoveryResponse(profile.frame(0u, response = true)))
        assertNull(PremiumProfile.discoveryResponse(profile.frame(1u, response = true)))
        assertNull(PremiumProfile.discoveryResponse(profile.frame(0u, response = false)))
    }
    @Test fun discoveryRejectsMalformedResponsesAndOtherExtensionActions() {
        val reply = PremiumProfile().frame(0u, response = true)
        for (length in 0 until reply.size) assertNull(PremiumProfile.discoveryResponse(reply.copyOf(length)))
        assertNull(PremiumProfile.discoveryResponse(reply + 0))
        assertNull(PremiumProfile.discoveryResponse(reply.copyOf().also { it[0] = 2 }))
        assertNull(PremiumProfile.discoveryResponse(reply.copyOf().also { it[1] = 0x72 }))
        assertNull(PremiumProfile.discoveryResponse(reply.copyOf().also { it[6] = 3 }))
        assertNull(PremiumProfile.discoveryResponse(reply.copyOf().also { it[47] = 1 }))
    }
    @Test fun roundTripsEveryFaceAndDisplayFlag() {
        for (face in 0..2) for (flags in 0..7) {
            val p = PremiumProfile(face = face, alwaysOn = flags and 1 != 0,
                largeText = flags and 2 != 0, reduceMotion = flags and 4 != 0,
                deckOrder = listOf(4, 3, 2, 1, 0), deckMask = 4)
            assertEquals(p, PremiumProfile.fromJson(p.json()))
            assertEquals(p, PremiumProfile.parseFrame(p.frame(1u, true))!!.second)
        }
    }
    @Test fun rejectsUnknownVersionAndFields() {
        val json = PremiumProfile().json()
        listOf(json.replace("\"version\": 1", "\"version\": 2"),
            json.replace("\"version\": 1", "\"version\": 1, \"script\": \"anything\""),
            "x".repeat(8193), "[".repeat(100) + "0" + "]".repeat(100))
            .forEach { assertTrue(runCatching { PremiumProfile.fromJson(it) }.isFailure) }
    }
    @Test fun rejectsUnsafeOrAmbiguousProfiles() {
        listOf(PremiumProfile(name = ""), PremiumProfile(name = "a".repeat(25)),
            PremiumProfile(name = "hello\nworld"), PremiumProfile(accent = 0x111111),
            PremiumProfile(accent = 0x1000000), PremiumProfile(face = 3),
            PremiumProfile(deckMask = 0), PremiumProfile(deckOrder = listOf(0, 0, 2, 3, 4)),
            PremiumProfile(complications = listOf(0, 1, 8))).forEach {
            assertTrue(runCatching { it.validate() }.isFailure)
        }
    }
    @Test fun rejectsTruncationAndTrailingData() {
        val bytes = PremiumProfile().frame(1u)
        for (length in 0 until bytes.size) assertNull(PremiumProfile.parseFrame(bytes.copyOf(length)))
        assertNull(PremiumProfile.parseFrame(bytes + 0))
        bytes[47] = 1; assertNull(PremiumProfile.parseFrame(bytes))
    }
    @Test fun backupDoesNotContainCredentials() {
        val json = PremiumProfile().json().lowercase()
        listOf("password", "token", "secret", "ssid", "credential", "address", "gateway").forEach { assertFalse(json.contains(it)) }
    }
}
