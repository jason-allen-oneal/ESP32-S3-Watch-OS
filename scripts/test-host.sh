#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-gyro-test.XXXXXX")"
trap 'rm -f "${test_binary}"' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/gyro_processor.cpp" \
  "${project_dir}/tests/gyro_processor_test.cpp" \
  -o "${test_binary}"
"${test_binary}"

printf 'Nightglass host tests passed\n'
