#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sudo -n true

if ! getent passwd nightglass-sign >/dev/null; then
  sudo useradd --system --home-dir /var/lib/nightglass-sign \
    --create-home --shell /usr/sbin/nologin nightglass-sign
fi
sudo install -d -o root -g root -m 700 /etc/credstore.encrypted
sudo install -d -o nightglass-sign -g nightglass-sign -m 700 \
  /var/lib/nightglass-sign/inbox /var/lib/nightglass-sign/outbox
sudo install -o root -g root -m 755 \
  "${project_dir}/packaging/systemd/nightglass-sign-helper" \
  /usr/local/libexec/nightglass-sign
sudo install -o root -g root -m 644 \
  "${project_dir}/packaging/systemd/nightglass-sign.service" \
  /etc/systemd/system/nightglass-sign.service
sudo systemctl daemon-reload
sudo systemd-analyze security nightglass-sign.service --no-pager >/dev/null
printf 'Nightglass signing service installed\n'

