#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_image="${project_dir}/components/nightglass_ui/assets/source/revenant_grid_v2_concept.png"
production_png="${project_dir}/components/nightglass_ui/assets/revenant_grid_v2.png"
production_rgb565="${project_dir}/components/nightglass_ui/assets/revenant_grid_v2.rgb565"
expected_source_sha="0cba3f7900708813eeea1b46325a04975872aa13ad224522b949687fa1dfdee4"
expected_bytes=$((410 * 502 * 2))
mode="${1:-write}"

if [[ "${mode}" != "write" && "${mode}" != "--check" ]]; then
    echo "usage: $0 [--check]" >&2
    exit 2
fi

command -v magick >/dev/null || {
    echo "ImageMagick 7 (magick) is required" >&2
    exit 1
}

actual_source_sha="$(sha256sum "${source_image}" | awk '{print $1}')"
if [[ "${actual_source_sha}" != "${expected_source_sha}" ]]; then
    echo "source concept checksum mismatch" >&2
    exit 1
fi

temporary_dir="$(mktemp -d "${TMPDIR:-/tmp}/nightglass-revenant-v2.XXXXXX")"
trap 'rm -rf -- "${temporary_dir}"' EXIT
generated_png="${temporary_dir}/revenant_grid_v2.png"
generated_rgb565="${temporary_dir}/revenant_grid_v2.rgb565"

magick "${source_image}" \
    -colorspace sRGB \
    -filter Lanczos \
    -resize '410x502!' \
    -strip \
    -define png:color-type=2 \
    -define png:compression-filter=5 \
    -define png:compression-level=9 \
    -define png:compression-strategy=1 \
    "${generated_png}"

dimensions="$(magick identify -format '%wx%h' "${generated_png}")"
if [[ "${dimensions}" != "410x502" ]]; then
    echo "generated image has unexpected dimensions: ${dimensions}" >&2
    exit 1
fi

magick "${generated_png}" -depth 8 rgb:- | perl -e '
    use strict;
    use warnings;
    local $/;
    my $rgb = <STDIN>;
    die "incomplete RGB stream\n" if length($rgb) % 3;
    my $output = "";
    for (my $index = 0; $index < length($rgb); $index += 3) {
        my ($red, $green, $blue) = unpack("C3", substr($rgb, $index, 3));
        my $pixel = (($red >> 3) << 11) | (($green >> 2) << 5) | ($blue >> 3);
        $output .= pack("v", $pixel);
    }
    print $output;
' >"${generated_rgb565}"

actual_bytes="$(wc -c <"${generated_rgb565}")"
if [[ "${actual_bytes}" -ne "${expected_bytes}" ]]; then
    echo "RGB565 output has unexpected size: ${actual_bytes}" >&2
    exit 1
fi

if [[ "${mode}" == "--check" ]]; then
    cmp --silent "${generated_png}" "${production_png}" || {
        echo "production PNG is stale" >&2
        exit 1
    }
    cmp --silent "${generated_rgb565}" "${production_rgb565}" || {
        echo "production RGB565 asset is stale" >&2
        exit 1
    }
    echo "Revenant Grid v2 assets are reproducible"
    exit 0
fi

install -m 0644 "${generated_png}" "${production_png}"
install -m 0644 "${generated_rgb565}" "${production_rgb565}"
sha256sum "${production_png}" "${production_rgb565}"
