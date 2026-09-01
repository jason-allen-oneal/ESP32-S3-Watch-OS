#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_dir="${NIGHTGLASS_IDF_PATH:-${IDF_PATH:-${HOME}/esp/esp-idf-v5.5.5}}"

"${project_dir}/scripts/verify-idf.sh" "${idf_dir}"
python3 "${project_dir}/scripts/verify-partitions.py" "${project_dir}/partitions.csv" >/dev/null

if [[ -n "$(git -C "${project_dir}" status --porcelain)" ]]; then
  echo "Release verification requires a clean Git worktree" >&2
  git -C "${project_dir}" status --short >&2
  exit 1
fi

# The pinned absolute path is checked above.
# shellcheck disable=SC1091
source "${idf_dir}/export.sh" >/dev/null
"${project_dir}/scripts/test-host.sh" >/dev/null
"${project_dir}/scripts/build.sh" >/dev/null

artifacts=(
  "build/bootloader/bootloader.bin"
  "build/partition_table/partition-table.bin"
  "build/ota_data_initial.bin"
  "build/nightglass.bin"
  "build/assets.bin"
)

for artifact in "${artifacts[@]}"; do
  test -s "${project_dir}/${artifact}"
done

first_hashes="$(cd "${project_dir}" && sha256sum "${artifacts[@]}")"
"${project_dir}/scripts/build.sh" >/dev/null
second_hashes="$(cd "${project_dir}" && sha256sum "${artifacts[@]}")"

if [[ "${first_hashes}" != "${second_hashes}" ]]; then
  echo "Deterministic double-build check failed" >&2
  diff <(printf '%s\n' "${first_hashes}") <(printf '%s\n' "${second_hashes}") >&2 || true
  exit 1
fi

python "${idf_dir}/components/partition_table/gen_esp32part.py" \
  "${project_dir}/build/partition_table/partition-table.bin" >/dev/null
python -m esptool --chip esp32s3 image_info "${project_dir}/build/nightglass.bin" >/dev/null

app_size="$(stat -c '%s' "${project_dir}/build/nightglass.bin")"
app_limit=$((6 * 1024 * 1024))
if (( app_size > app_limit )); then
  echo "Application exceeds the 6 MiB OTA slot" >&2
  exit 1
fi

python3 "${project_dir}/scripts/validate-runtime-assets.py" \
  "${project_dir}/runtime_assets" >/dev/null
assets_size="$(stat -c '%s' "${project_dir}/build/assets.bin")"
assets_limit=$((0x1360000))
if (( assets_size != assets_limit )); then
  echo "Runtime asset image must exactly fill its reviewed LittleFS partition" >&2
  echo "assets_size=${assets_size} expected=${assets_limit}" >&2
  exit 1
fi

printf 'Nightglass release gate passed\n'
printf 'commit %s\n' "$(git -C "${project_dir}" rev-parse HEAD)"
printf 'app_size %s/%s bytes\n' "${app_size}" "${app_limit}"
printf 'assets_size %s/%s bytes\n' "${assets_size}" "${assets_limit}"
printf '%s\n' "${second_hashes}"
