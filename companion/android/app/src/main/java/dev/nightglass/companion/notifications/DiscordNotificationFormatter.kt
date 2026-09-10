package dev.nightglass.companion.notifications

/**
 * Converts the several layouts used by Discord's Android notifications into
 * one bounded, watch-friendly sender/context + message pair. This is only
 * presentation data; it never inspects or stores Discord account credentials.
 */
internal object DiscordNotificationFormatter {
    data class Message(val title: String, val body: String)

    fun format(rawTitle: String, conversation: String, subText: String,
               body: String, latestMessage: String): Message {
        val title = compact(rawTitle)
        val context = listOf(compact(conversation), compact(subText))
            .filter { it.isNotEmpty() }
            .distinct()
            .joinToString(" / ")
        val displayTitle = when {
            title.isEmpty() -> context
            title.equals("Discord", ignoreCase = true) && context.isNotEmpty() -> context
            context.isEmpty() || title.contains(context) -> title
            else -> "$title | $context"
        }
        val displayBody = compact(body).ifEmpty { compact(latestMessage) }
        return Message(displayTitle, displayBody)
    }

    private fun compact(value: String): String = value
        .replace(Regex("\\s+"), " ")
        .trim()
}
