#!/usr/bin/env python3
"""Guard token-safe companion recovery across Android lifecycle events."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = (ROOT / "companion/android/app/src/main/AndroidManifest.xml").read_text()
RECEIVER = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/ble/NightglassLifecycleReceiver.kt").read_text()
SERVICE = (ROOT / "companion/android/app/src/main/java/dev/nightglass/companion/ble/NightglassConnectionService.kt").read_text()


assert 'android.permission.RECEIVE_BOOT_COMPLETED' in MANIFEST
assert '.ble.NightglassLifecycleReceiver' in MANIFEST
assert 'android.intent.action.BOOT_COMPLETED' in MANIFEST
assert 'android.intent.action.MY_PACKAGE_REPLACED' in MANIFEST
assert 'getSharedPreferences' in RECEIVER
assert 'PINNED_ADDRESS' in RECEIVER
assert 'startForegroundService' in RECEIVER
assert 'ACTION_CONNECT' in RECEIVER
assert 'BluetoothAdapter.ACTION_STATE_CHANGED' in SERVICE
assert 'STATE_TURNING_OFF' in SERVICE
assert 'Bluetooth on; reconnecting' in SERVICE
assert 'Bluetooth off; waiting for adapter' in SERVICE
assert 'unregisterReceiver(adapterStateReceiver)' in SERVICE

# An old Android service cache must not require unpairing. Probe only after
# normal authorization, and accept readback only from the current bearer.
authorized = SERVICE[SERVICE.index('private fun completeAuthorizedLink'):SERVICE.index('private fun stopScan')]
assert 'premiumProfileWrites.addFirst(byteArrayOf(1, 0x71))' in authorized
assert 'if (premiumSupported) writes.addFirst' not in authorized
receive = SERVICE[SERVICE.index('private fun receive(client:'):SERVICE.index('private fun receivePremium')]
assert 'gatt !== client' in receive
assert 'characteristic.uuid != NightglassProtocol.WATCH_TO_PHONE' in receive
assert 'generation != linkGeneration' in receive
assert 'PremiumProfile.discoveryResponse(owned)' in receive

# Notification queue overflow and rich-content replacement must not discard a
# settings query/save. Keep those frames bounded and ahead of ordinary traffic.
write = SERVICE[SERVICE.index('private fun write(frame:'):SERVICE.index('private fun writeOta')]
assert write.index('premiumProfileWrites.add(frame.copyOf())') < write.index('writes.removeFirst()')
assert 'premiumProfileWrites.size >= 8' in write
drain = SERVICE[SERVICE.index('private fun writeNextLocked'):SERVICE.index('private fun requestAuthorizationProof')]
assert 'voiceEntry?.frame ?: profileFrame ?: premiumFrame ?: writes.peek()' in drain
reset = SERVICE[SERVICE.index('private fun resetLinkState'):SERVICE.index('private fun recoverDeadLink')]
assert 'premiumProfileWrites.forEach { it.fill(0) }; premiumProfileWrites.clear()' in reset

print("Nightglass companion lifecycle recovery contract passed")
