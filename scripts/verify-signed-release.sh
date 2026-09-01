#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output="${1:-${project_dir}/build/update-package}"

"${project_dir}/scripts/verify-release.sh"
"${project_dir}/scripts/sign-update.sh" \
  "${project_dir}/build/nightglass.bin" "${output}"
python3 "${project_dir}/scripts/verify-signed-package.py" "${output}"
sha256sum "${output}/firmware.bin" "${output}/manifest.payload" \
  "${output}/manifest.sig"

