#!/usr/bin/env python3
"""Guard the Discord inbox/detail interaction contract."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHELL = (ROOT / "components/nightglass_ui/src/shell.cpp").read_text()
LISTENER = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/notifications/NightglassNotificationListener.kt").read_text()
FORMATTER = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/notifications/DiscordNotificationFormatter.kt").read_text()
SHARE = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/notifications/DiscordVoiceShare.kt").read_text()
SERVICE = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/ble/NightglassConnectionService.kt").read_text()
PROTOCOL = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/protocol/NightglassProtocol.kt").read_text()


for text in (SHELL, LISTENER, FORMATTER, SHARE, SERVICE):
    assert "Discord" in text

assert '"DISCORD MESSAGE"' in SHELL
assert '"REPLY"' in SHELL
assert '"VOICE"' in SHELL
assert '"OPEN"' in SHELL
assert '"CLEAR INBOX"' in SHELL
assert 'discord_notification_kind' in SHELL
assert 'Notification.EXTRA_CONVERSATION_TITLE' in LISTENER
assert 'Notification.EXTRA_MESSAGES' in LISTENER
assert 'DiscordNotificationFormatter.format' in LISTENER
assert 'latestMessage' in FORMATTER
assert 'no Discord token is stored here' in SHELL
assert '"HOLD TO RECORD"' in SHELL
assert 'VoiceDestination::discord_voice_note' in SHELL
assert 'VOICE_REQUEST_FLAG_DISCORD_REPLY' in PROTOCOL
assert 'activeVoiceDiscordReply' in SERVICE
assert 'FileProvider.getUriForFile' in SHARE
assert 'setPackage(DISCORD_PACKAGE)' in SHARE
assert 'VOICE NOTE READY - CONFIRM SEND IN DISCORD' in SERVICE
assert 'voiceStatus(owner.watchSession, 8)' in SERVICE

print("Nightglass Discord surface contract passed")
