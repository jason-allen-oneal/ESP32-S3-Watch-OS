#!/usr/bin/env bash
set -euo pipefail

cat >&2 <<'EOF'
BLOCKED: generic ESP-IDF flash targets are not a Nightglass release path.

These targets can overwrite the bootloader, partition table, OTA selection,
runtime assets, or the wrong application slot. This repository intentionally
adds a failing dependency to the upstream targets; it does not remove them.

Run scripts/preflight-app-flash.sh first for read-only target/recovery checks.
Nightglass still has no production USB deployment command. Use the reviewed
inactive-slot UpdateService transport once its signing key and transport are
provisioned, or perform an explicitly reviewed recovery procedure.
EOF
exit 1
