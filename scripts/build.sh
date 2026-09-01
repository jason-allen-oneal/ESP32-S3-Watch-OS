#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_dir="${NIGHTGLASS_IDF_PATH:-${IDF_PATH:-${HOME}/esp/esp-idf-v5.5.5}}"

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
  "${project_dir}/sdkconfig" >/dev/null
idf.py -C "${project_dir}" build
python3 "${project_dir}/scripts/verify-release-config.py" \
  "${project_dir}/build/config/sdkconfig.h" >/dev/null
cat <<'EOF'
Nightglass build complete (no device write).
IGNORE ESP-IDF's generic flash suggestions above: repository flash targets are
blocked by policy. Run scripts/preflight-app-flash.sh for read-only checks.
EOF
