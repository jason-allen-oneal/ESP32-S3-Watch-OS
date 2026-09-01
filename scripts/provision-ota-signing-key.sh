#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
credential=/etc/credstore.encrypted/nightglass-ota-signing-key
public_output="${1:-/tmp/nightglass-ota-public-key.hex}"
recovery_dir="/home/rev/projects/watch/backups"
recovery_copy="${recovery_dir}/nightglass-ota-signing-key-recovery-20260831.pk8"
temporary="$(mktemp -d /tmp/nightglass-key-ceremony.XXXXXX)"
trap 'find "${temporary}" -type f -delete; rmdir "${temporary}"' EXIT
umask 077

ask_secret() {
  local prompt="$1"
  if [[ -n "${DISPLAY:-}" ]] && command -v zenity >/dev/null 2>&1; then
    zenity --password --title="Nightglass signing key" --text="${prompt}"
  else
    systemd-ask-password --no-tty --timeout=300 "${prompt}"
  fi
}

"${project_dir}/scripts/install-ota-signing-service.sh" >/dev/null
if sudo test -e "${credential}"; then
  echo "Production signing credential already exists; refusing replacement" >&2
  exit 17
fi
if [[ -e "${recovery_copy}" ]]; then
  echo "Recovery key copy already exists; refusing replacement" >&2
  exit 17
fi

first="$(ask_secret "Create Nightglass OTA signing passphrase")"
second="$(ask_secret "Confirm Nightglass OTA signing passphrase")"
if [[ -z "${first}" || "${first}" != "${second}" ]]; then
  unset first second
  echo "Signing passphrases did not match" >&2
  exit 1
fi
unset second

exec 3<<<"${first}"
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 2>/dev/null |
  openssl pkcs8 -topk8 -v2 aes-256-cbc -iter 600000 -passout fd:3 \
    -out "${temporary}/encrypted.pk8"
exec 3<&-

exec 3<<<"${first}"
openssl pkey -in "${temporary}/encrypted.pk8" -passin fd:3 \
  -pubout -outform DER 2>/dev/null |
  python3 -c 'import sys; data=sys.stdin.buffer.read(); point=data[-65:]; assert len(point)==65 and point[0]==4; print(point.hex())' \
  > "${temporary}/public.hex"
exec 3<&-
unset first

systemd-creds --with-key=host+tpm2 --name=nightglass-ota-signing-key \
  encrypt "${temporary}/encrypted.pk8" "${temporary}/nightglass-ota-signing-key" >/dev/null
sudo install -o root -g root -m 600 \
  "${temporary}/nightglass-ota-signing-key" "${credential}"
install -d -m 700 "${recovery_dir}"
install -m 600 "${temporary}/encrypted.pk8" "${recovery_copy}"
install -m 644 "${temporary}/public.hex" "${public_output}"

printf 'Production signing credential sealed with host+TPM2\n'
printf 'Encrypted recovery copy: %s\n' "${recovery_copy}"
printf 'Public SEC1 point: %s\n' "${public_output}"
