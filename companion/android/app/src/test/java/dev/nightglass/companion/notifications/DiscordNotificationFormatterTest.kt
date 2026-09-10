package dev.nightglass.companion.notifications

import org.junit.Assert.assertEquals
import org.junit.Test

class DiscordNotificationFormatterTest {
    @Test fun includesConversationContextWithoutLosingSender() {
        val message = DiscordNotificationFormatter.format(
            "Mina", "#builds", "Nightglass", "ship it", "")
        assertEquals("Mina | #builds / Nightglass", message.title)
        assertEquals("ship it", message.body)
    }

    @Test fun replacesGenericDiscordTitleWithConversation() {
        val message = DiscordNotificationFormatter.format(
            "Discord", "#general", "", "", "hello from the thread")
        assertEquals("#general", message.title)
        assertEquals("hello from the thread", message.body)
    }

    @Test fun fallsBackToLatestMessageWhenBodyIsMissing() {
        val message = DiscordNotificationFormatter.format(
            "Mina", "", "", "  ", "latest message")
        assertEquals("Mina", message.title)
        assertEquals("latest message", message.body)
    }

    @Test fun compactsMultilineNotificationTextForTheWatch() {
        val message = DiscordNotificationFormatter.format(
            "Mina", "#builds", "", "first line\nsecond line", "")
        assertEquals("Mina | #builds", message.title)
        assertEquals("first line second line", message.body)
    }
}
