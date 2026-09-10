#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_dir="${NIGHTGLASS_IDF_PATH:-${IDF_PATH:-${HOME}/esp/esp-idf-v5.5.5}}"
app_version="${NIGHTGLASS_PROJECT_VERSION:-}"
secure_version="${NIGHTGLASS_SECURE_VERSION:-}"

if [[ -n "${app_version}" || -n "${secure_version}" ]]; then
  if [[ ! "${app_version}" =~ ^[0-9A-Za-z][0-9A-Za-z.+_-]{0,30}$ ]]; then
    echo "NIGHTGLASS_PROJECT_VERSION must be a canonical 1-31 byte version" >&2
    exit 2
  fi
  if [[ ! "${secure_version}" =~ ^[1-9][0-9]*$ ]]; then
    echo "NIGHTGLASS_SECURE_VERSION must be a positive integer" >&2
    exit 2
  fi
fi

"${project_dir}/scripts/verify-idf.sh" "${idf_dir}"

# The pinned absolute path is checked above.
# shellcheck disable=SC1091
source "${idf_dir}/export.sh" >/dev/null

# set-target performs a full clean. Only use it to seed a fresh checkout;
# subsequent builds retain the verified sdkconfig and incremental cache.
if [[ ! -f "${project_dir}/sdkconfig" ]]; then
  idf.py -C "${project_dir}" set-target esp32s3
fi
python3 "${project_dir}/scripts/verify-release-config.py" \
  "${project_dir}/sdkconfig" --secure-version "${secure_version:-1}" >/dev/null
cmake_definitions=()
if [[ -n "${app_version}" ]]; then
  cmake_definitions+=(
    -D "PROJECT_VER=${app_version}"
    -D "NIGHTGLASS_APP_SECURE_VERSION=${secure_version}"
  )
fi
idf.py -C "${project_dir}" "${cmake_definitions[@]}" build
python3 "${project_dir}/scripts/verify-release-config.py" \
  "${project_dir}/build/config/sdkconfig.h" \
  --secure-version "${secure_version:-1}" >/dev/null
if [[ -n "${app_version}" ]]; then
  python3 "${project_dir}/scripts/verify-app-image.py" \
    "${project_dir}/build/nightglass.bin" --version "${app_version}" \
    --secure-version "${secure_version}" >/dev/null
fi
cat <<'EOF'
Nightglass build complete (no device write).
IGNORE ESP-IDF's generic flash suggestions above: repository flash targets are
blocked by policy. Run scripts/preflight-app-flash.sh for read-only checks.
EOF
