#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gyro_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-gyro-test.XXXXXX")"
time_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-time-test.XXXXXX")"
face_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-face-test.XXXXXX")"
connectivity_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-connectivity-test.XXXXXX")"
activity_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-activity-test.XXXXXX")"
activity_units_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-activity-units-test.XXXXXX")"
day_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-activity-day-test.XXXXXX")"
weather_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-weather-test.XXXXXX")"
navigation_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-navigation-test.XXXXXX")"
audio_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-audio-test.XXXXXX")"
update_binary="$(mktemp "${TMPDIR:-/tmp}/nightglass-update-test.XXXXXX")"
trap 'rm -f "${gyro_binary}" "${time_binary}" "${face_binary}" "${connectivity_binary}" "${activity_binary}" "${activity_units_binary}" "${day_binary}" "${weather_binary}" "${navigation_binary}" "${audio_binary}" "${update_binary}"' EXIT

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

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/connectivity_protocol.cpp" \
  "${project_dir}/tests/connectivity_protocol_test.cpp" \
  -o "${connectivity_binary}"
"${connectivity_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/activity_processor.cpp" \
  "${project_dir}/tests/activity_processor_test.cpp" \
  -o "${activity_binary}"
"${activity_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/activity_units.cpp" \
  "${project_dir}/tests/activity_units_test.cpp" \
  -o "${activity_units_binary}"
"${activity_units_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/activity_day.cpp" \
  "${project_dir}/tests/activity_day_test.cpp" \
  -o "${day_binary}"
"${day_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  "${project_dir}/components/nightglass_services/src/weather_logic.cpp" \
  "${project_dir}/tests/weather_logic_test.cpp" \
  -o "${weather_binary}"
"${weather_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_core/include" \
  "${project_dir}/components/nightglass_core/src/navigation.cpp" \
  "${project_dir}/tests/navigation_test.cpp" \
  -o "${navigation_binary}"
"${navigation_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_services/include" \
  -I"${project_dir}/components/nightglass_core/include" \
  "${project_dir}/components/nightglass_services/src/audio.cpp" \
  "${project_dir}/tests/audio_test.cpp" \
  -o "${audio_binary}"
"${audio_binary}"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
  -I"${project_dir}/components/nightglass_update/include" \
  "${project_dir}/components/nightglass_update/src/manifest.cpp" \
  "${project_dir}/components/nightglass_update/src/policy.cpp" \
  "${project_dir}/tests/update_manifest_test.cpp" \
  -o "${update_binary}"
"${update_binary}"

printf 'Nightglass host tests passed\n'
