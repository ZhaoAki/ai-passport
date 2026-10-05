#!/bin/bash
# Launch an owner-operated LAN test server. No fixed passwords are embedded.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
if ! command -v python3 >/dev/null 2>&1; then
    echo 'Python 3.9 or newer is required.'
    read -r -p 'Press Return to close.' unused
    exit 1
fi
python3 -c 'import sys; assert sys.version_info >= (3, 9), "Python 3.9 or newer is required"'
export PYTHONPATH="$repo_root/services"
export LANLAN_DB="$HOME/Library/Application Support/CyberLanlan/lanlan.sqlite3"
export LANLAN_WEB_DIR="$repo_root/web/lanlan"
export LANLAN_ENV=development
export LANLAN_SECURE_COOKIES=0
mkdir -p "$(dirname -- "$LANLAN_DB")"
umask 077
if [[ ! -s "$LANLAN_DB" ]]; then
    python3 -m lanlan init
    echo 'Save the generated caregiver passwords above before continuing.'
    read -r -p 'Press Return after saving them.' unused
fi
printf '\nDatabase: %s\n' "$LANLAN_DB"
echo 'Phone and Passport must use this computer LAN address, not 127.0.0.1.'
for interface in en0 en1; do
    lanlan_ip="$(ipconfig getifaddr "$interface" 2>/dev/null || true)"
    if [[ -n "$lanlan_ip" ]]; then
        printf 'Phone URL: http://%s:8787/\n' "$lanlan_ip"
    fi
done
echo 'Keep this terminal open during testing. Ctrl-C stops the server.'
exec python3 -m lanlan serve --host 0.0.0.0 --port 8787
