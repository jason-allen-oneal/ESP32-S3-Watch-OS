#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
BLOCKED: generic ESP-IDF flash targets are not a Nightglass release path.

These targets can overwrite the bootloader, partition table, OTA selection,
runtime assets, or the wrong application slot. This repository intentionally
adds a failing dependency to the upstream targets; it does not remove them.

For a direct watch-cable install, use the signed on-device updater:

  python3 scripts/nightglass-usb-update.py SIGNED_PACKAGE \
    --device /dev/serial/by-id/usb-Espressif_...

The companion's signed-package picker remains the OTA path. Both supported
methods feed UpdateService and preserve inactive-slot validation and rollback.
EOF
exit 1
