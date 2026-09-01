#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 )); then
  echo "usage: $0 /dev/serial/by-id/... /absolute/path/full-flash.bin EXPECTED_SHA256" >&2
  echo "Read-only preflight; this script never writes the watch." >&2
  exit 2
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
serial_path="$1"
recovery_image="$2"
expected_recovery_sha="${3,,}"
idf_dir="${NIGHTGLASS_IDF_PATH:-${IDF_PATH:-${HOME}/esp/esp-idf-v5.5.5}}"

case "${serial_path}" in
  /dev/serial/by-id/*) ;;
  *) echo "target must use a stable /dev/serial/by-id path" >&2; exit 1 ;;
esac
[[ -c "${serial_path}" ]] || { echo "stable serial target is not present" >&2; exit 1; }
[[ "${recovery_image}" = /* ]] || { echo "recovery image path must be absolute" >&2; exit 1; }
[[ -f "${recovery_image}" ]] || { echo "recovery image is missing" >&2; exit 1; }
[[ "${expected_recovery_sha}" =~ ^[0-9a-f]{64}$ ]] || {
  echo "expected recovery SHA-256 must be 64 lowercase/uppercase hex characters" >&2
  exit 1
}

recovery_size="$(stat -c '%s' "${recovery_image}")"
recovery_mode="$(stat -c '%a' "${recovery_image}")"
if [[ "${recovery_size}" != "33554432" ]]; then
  echo "recovery image must be exactly 32 MiB" >&2
  exit 1
fi
if (( (8#${recovery_mode} & 8#077) != 0 )); then
  echo "recovery image must be owner-only (mode 0600)" >&2
  exit 1
fi
actual_recovery_sha="$(sha256sum "${recovery_image}" | awk '{print $1}')"
[[ "${actual_recovery_sha}" == "${expected_recovery_sha}" ]] || {
  echo "recovery image SHA-256 mismatch" >&2
  exit 1
}

"${project_dir}/scripts/verify-idf.sh" "${idf_dir}"
# shellcheck disable=SC1091
source "${idf_dir}/export.sh" >/dev/null
python3 "${project_dir}/scripts/verify-partitions.py" "${project_dir}/partitions.csv" >/dev/null
python3 "${project_dir}/scripts/verify-release-config.py" "${project_dir}/sdkconfig" >/dev/null
python3 "${project_dir}/scripts/verify-release-config.py" \
  "${project_dir}/build/config/sdkconfig.h" >/dev/null
test -s "${project_dir}/build/nightglass.bin"
python -m esptool --chip esp32s3 image_info \
  "${project_dir}/build/nightglass.bin" >/dev/null

# Query identity only. These commands may reset the target but never erase or
# write flash. The stable path is resolved again between probes.
python -m esptool --chip esp32s3 --port "${serial_path}" chip_id >/dev/null
[[ -c "${serial_path}" ]] || { echo "target did not re-enumerate after chip probe" >&2; exit 1; }
flash_id_output="$(python -m esptool --chip esp32s3 --port "${serial_path}" flash_id 2>&1)"
grep -Eq 'Detected flash size:[[:space:]]*32MB' <<<"${flash_id_output}" || {
  echo "connected ESP32-S3 did not report 32 MB flash" >&2
  exit 1
}

printf 'Nightglass read-only application preflight passed\n'
printf 'target %s\n' "${serial_path}"
printf 'recovery_sha256 %s\n' "${actual_recovery_sha}"
printf 'app_sha256 %s\n' "$(sha256sum "${project_dir}/build/nightglass.bin" | awk '{print $1}')"
printf 'NO FLASH WAS PERFORMED\n'
