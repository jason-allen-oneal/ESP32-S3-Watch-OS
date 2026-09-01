#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
serial_path="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_3C:DC:75:6E:F0:2C-if00"
recovery="/home/rev/projects/watch/backups/nightglass-pre-ota-bootstrap-20260831-full-32m.bin"
recovery_manifest="${recovery}.sha256"

[[ -L "${serial_path}" || -c "${serial_path}" ]]
[[ -f "${recovery}" && "$(stat -c '%s' "${recovery}")" -eq 33554432 ]]
[[ "$(stat -c '%a' "${recovery}")" == "600" ]]
[[ -f "${recovery_manifest}" && "$(stat -c '%a' "${recovery_manifest}")" == "600" ]]
sha256sum -c "${recovery_manifest}" >/dev/null
sudo -n test -r /etc/credstore.encrypted/nightglass-ota-signing-key
"${project_dir}/scripts/test-host.sh" >/dev/null
"${project_dir}/scripts/verify-release.sh" >/dev/null
python3 "${project_dir}/scripts/inspect-ota-state.py" "${recovery}" \
  --partition-table "${project_dir}/build/partition_table/partition-table.bin" \
  --candidate "${project_dir}/build/nightglass.bin" >/dev/null
printf 'Nightglass OTA HIL preflight passed\n'

