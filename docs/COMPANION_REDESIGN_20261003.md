# Companion redesign — 2026-10-03

The owner approved the interactive visual concept before implementation. Companion version 0.2.0 (version code 2) now launches the redesigned Home screen; the existing watch control center remains reachable from Watch.

## Implemented

- Home / Watch / Settings navigation and dark card layout with readable touch targets.
- Separate watch and OpenClaw status; OpenClaw uses timestamped, authenticated health results rather than treating Bluetooth as Gateway connectivity.
- Guided connection recovery, setup QR flow, and actionable offline/access messages.
- City/postal-code weather search with explicit place selection; coordinates, refresh interval, and optional Wi-Fi hidden under advanced options.
- Existing weather values loaded; reset/credential removal hidden under advanced connection options.
- Calendar and call permissions requested separately from pairing; denying notification permission does not block Bluetooth.
- Existing credentials, BLE pinning, signed update validation, and native watch control center retained.

## Verification

- Debug APK assembled; Android APK signature verification passed.
- 83 unit tests passed, zero failures/errors/skips.
- Android lint: 0 errors, 0 fatal issues, 54 warnings.
- git diff --check passed.
- SHA-256: 2fea99b3eae580743293faff60533a15b9f02a4eb19fedb8aa2a0d91bae99947.

## Remaining device verification

No phone is connected to ADB. Nothing was installed. Before an in-place update, compare installed signing identity with the APK; do not uninstall or clear data to bypass a mismatch. Check rendering, navigation, permission denial, live OpenClaw recovery, weather search, and update progress on the phone. Watch appearance controls retain the existing native control center rather than the mockup's simulated face gallery.

## Phone installation and smoke check

Installed 0.2.0 / version code 2 in place on the Galaxy S23 with adb install -r. Installed and new APK signing certificate SHA-256 matched (c8ad213a63f65cf74842424cb9166156f0b129bcf45b0fd3d7792962e8ad7e74). Android firstInstallTime remains 2026-08-30 17:47:57. No uninstall, data clear, pairing reset, or QR reprovisioning performed.

Watch reauthorized its existing link. Visually inspected Home, Watch, Settings, and the recovery dialog; tab navigation and recovery opening work. Weather lookup, permission-denial flows, and firmware update were not exercised on-device.

Existing OpenClaw failure is confirmed as configured=true, internet=true, reachable=false; operator_transport throws UnknownHostException/GaiException. The configured hostname cannot resolve on the phone; no Gateway URL or credentials were changed in this update.
