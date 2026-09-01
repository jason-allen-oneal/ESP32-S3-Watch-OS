#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <nightglass.bin> <output-directory>" >&2
  exit 2
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="$(realpath "$1")"
output="$(realpath -m "$2")"
staging="$(mktemp -d /tmp/nightglass-sign.XXXXXX)"
passphrase_fifo=/run/nightglass-sign/passphrase.fifo
cleanup() {
  sudo systemctl stop nightglass-sign.service >/dev/null 2>&1 || true
  sudo rm -f /var/lib/nightglass-sign/inbox/manifest.payload \
    /var/lib/nightglass-sign/outbox/manifest.sig \
    /var/lib/nightglass-sign/outbox/manifest.sig.tmp \
    "${passphrase_fifo}"
  find "${staging}" -depth -delete
}
trap cleanup EXIT
umask 077

ask_secret() {
  if [[ -n "${DISPLAY:-}" ]] && command -v zenity >/dev/null 2>&1; then
    zenity --password --title="Nightglass signing key" \
      --text="Enter the passphrase created during the Nightglass OTA key ceremony"
  else
    systemd-ask-password --no-tty --timeout=300 \
      "Nightglass OTA signing key passphrase"
  fi
}

sudo -n test -r /etc/credstore.encrypted/nightglass-ota-signing-key
python3 "${project_dir}/scripts/package-update.py" "${image}" \
  "${staging}/unsigned" >/dev/null
sudo install -o nightglass-sign -g nightglass-sign -m 600 \
  "${staging}/unsigned/manifest.payload" \
  /var/lib/nightglass-sign/inbox/manifest.payload
sudo rm -f /var/lib/nightglass-sign/outbox/manifest.sig
sudo install -d -o nightglass-sign -g nightglass-sign -m 700 \
  /run/nightglass-sign
sudo rm -f "${passphrase_fifo}"
sudo -u nightglass-sign mkfifo -m 600 "${passphrase_fifo}"
sudo systemctl start --no-block nightglass-sign.service
if [[ -n "${DISPLAY:-}" ]] && command -v zenity >/dev/null 2>&1; then
  timeout 300 bash -c '
    zenity --password --title="Nightglass signing key" \
      --text="Enter the passphrase created during the Nightglass OTA key ceremony" |
      sudo -u nightglass-sign tee "$1" >/dev/null
  ' _ "${passphrase_fifo}" &
else
  timeout 300 bash -c '
    systemd-ask-password --no-tty --timeout=300 \
      "Nightglass OTA signing key passphrase" |
      sudo -u nightglass-sign tee "$1" >/dev/null
  ' _ "${passphrase_fifo}" &
fi
secret_writer_pid=$!

# Credential setup occurs before ExecStart opens the FIFO. Observe the unit in
# parallel so a credential or sandbox failure cannot strand the caller on the
# FIFO until the human prompt timeout expires.
for _ in {1..3000}; do
  state="$(sudo systemctl show nightglass-sign.service \
    --property=ActiveState --value)"
  if [[ "${state}" == "failed" ]]; then
    kill "${secret_writer_pid}" >/dev/null 2>&1 || true
    wait "${secret_writer_pid}" >/dev/null 2>&1 || true
    echo "Nightglass signing service failed before accepting the passphrase" >&2
    exit 1
  fi
  kill -0 "${secret_writer_pid}" >/dev/null 2>&1 || break
  sleep 0.1
done
if kill -0 "${secret_writer_pid}" >/dev/null 2>&1; then
  kill "${secret_writer_pid}" >/dev/null 2>&1 || true
  wait "${secret_writer_pid}" >/dev/null 2>&1 || true
  echo "Nightglass signing passphrase prompt timed out" >&2
  exit 1
fi
if ! wait "${secret_writer_pid}"; then
  echo "Nightglass signing passphrase delivery failed" >&2
  exit 1
fi

for _ in {1..3000}; do
  state="$(sudo systemctl show nightglass-sign.service \
    --property=ActiveState --value)"
  [[ "${state}" == "activating" ]] || break
  sleep 0.1
done
if [[ "$(sudo systemctl show nightglass-sign.service \
    --property=Result --value)" != "success" ]] || \
   [[ "$(sudo systemctl show nightglass-sign.service \
    --property=ExecMainStatus --value)" != "0" ]]; then
  echo "Nightglass signing service failed" >&2
  exit 1
fi
sudo install -o "$(id -un)" -g "$(id -gn)" -m 600 \
  /var/lib/nightglass-sign/outbox/manifest.sig "${staging}/manifest.sig"
sudo rm -f /var/lib/nightglass-sign/inbox/manifest.payload \
  /var/lib/nightglass-sign/outbox/manifest.sig "${passphrase_fifo}"
python3 "${project_dir}/scripts/normalize-ecdsa-p256-signature.py" \
  "${staging}/manifest.sig"

python3 "${project_dir}/scripts/package-update.py" "${image}" "${output}" \
  --signature "${staging}/manifest.sig" \
  --signature-algorithm ecdsa-p256-sha256-der --force >/dev/null

chmod 600 "${output}/firmware.bin" "${output}/manifest.json" \
  "${output}/manifest.payload" "${output}/manifest.sig"
python3 "${project_dir}/scripts/verify-signed-package.py" "${output}" >/dev/null
printf 'Signed Nightglass update package created: %s\n' "${output}"
