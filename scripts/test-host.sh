#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gyro_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-gyro-test.XXXXXX")"
time_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-time-test.XXXXXX")"
face_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-face-test.XXXXXX")"
trap 'rm -f "${gyro_binary}" "${time_binary}" "${face_binary}"' EXIT

python3 "${project_dir}/scripts/check-runtime-glyphs.py"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/gyro_processor.cpp" \
  "${project_dir}/tests/gyro_processor_test.cpp" \
  -o "${gyro_binary}"
"${gyro_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/time_math.cpp" \
  "${project_dir}/tests/time_math_test.cpp" \
  -o "${time_binary}"
"${time_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  -I"${project_dir}/components/nightglass_core/include" \
  "${project_dir}/components/nightglass_services/src/watchface_schema.cpp" \
  "${project_dir}/tests/watchface_schema_test.cpp" \
  -o "${face_binary}"
"${face_binary}"

printf 'Nightglass host tests passed\n'
