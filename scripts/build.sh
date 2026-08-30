#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_dir="__LOCAL_ESP_IDF_PATH__"

if [[ ! -f "${idf_dir}/export.sh" ]]; then
  echo "Pinned ESP-IDF v5.5.5 is not installed at ${idf_dir}" >&2
  exit 1
fi

source "${idf_dir}/export.sh" >/dev/null
idf.py -C "${project_dir}" set-target esp32s3
idf.py -C "${project_dir}" build
