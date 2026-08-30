#!/usr/bin/env bash
set -euo pipefail

idf_dir="${1:?ESP-IDF path required}"
expected_commit="b774170ff46c393eeb5e495ea37936038d3f4f4f"

if [[ ! -f "${idf_dir}/export.sh" ]]; then
  echo "Pinned ESP-IDF v5.5.5 is not installed at ${idf_dir}" >&2
  exit 1
fi

actual_commit="$(git -C "${idf_dir}" rev-parse HEAD 2>/dev/null || true)"
if [[ "${actual_commit}" != "${expected_commit}" ]]; then
  echo "ESP-IDF source is not the pinned v5.5.5 commit" >&2
  echo "expected ${expected_commit}" >&2
  echo "actual   ${actual_commit:-unavailable}" >&2
  exit 1
fi

if ! status_output="$(git -C "${idf_dir}" status --porcelain --untracked-files=normal 2>&1)"; then
  echo "Unable to inspect the pinned ESP-IDF worktree" >&2
  exit 1
fi
if [[ -n "${status_output}" ]]; then
  echo "Pinned ESP-IDF worktree must be clean" >&2
  exit 1
fi

if ! submodule_output="$(git -C "${idf_dir}" submodule status --recursive 2>&1)"; then
  echo "Unable to inspect pinned ESP-IDF submodules" >&2
  exit 1
fi
if grep -Eq '^[+U-]' <<<"${submodule_output}"; then
  echo "Pinned ESP-IDF submodules are missing or at the wrong revisions" >&2
  exit 1
fi
