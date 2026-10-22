#!/bin/bash
# Sets up, or updates, the bench on this Mac from the rocbotnet repositories:
#
#   - the hub and the game, installed editable from ../../hub: an edit there
#     takes effect on `roclink restart`;
#   - ~/roclink/etc from etc/, with the MQTT passwords from the gateway's vault;
#   - `roclink` on the PATH (~/bin/roclink, a link to ./roclink).
#
#   ./setup.sh             keeps ~/roclink/etc's hub.toml and game.toml if they exist
#   ./setup.sh --configs   writes them again from etc/ (the old ones are kept as .bak)
#
# Data (the databases) and logs stay in ~/roclink, outside git.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GW=$ROOT/rfgateways/roclink-gateway
R=$HOME/roclink
CONFIGS=${1:-}
export PATH=$HOME/.local/bin:$PATH

command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh
uv python install -q 3.12
mkdir -p "$R"/{etc,data,log,run,venv} "$HOME/bin"
chmod 700 "$R/etc"

echo "== the hub and the game, from $ROOT/hub"
for pair in hub:roclink-hub scavhunt:roclink-scavhunt; do
    name=${pair%%:*} repo=${pair#*:}
    [ -x "$R/venv/$name/bin/python" ] || uv venv -q -p 3.12 "$R/venv/$name"
    uv pip install -q -p "$R/venv/$name/bin/python" -e "$ROOT/hub/$repo[dev]"
done
uv pip install -q -p "$R/venv/scavhunt/bin/python" cryptography  # tools/scavbox.py

echo "== secrets, from $GW/group_vars/all/vault.yml"
[ -f "$GW/.vault-pass" ] || { echo "no $GW/.vault-pass: copy it from a machine that has it"; exit 1; }
VAULT=$(cd "$GW" && ansible-vault view group_vars/all/vault.yml)
secret() { printf '%s\n' "$VAULT" | sed -n "s/^$1: *\"\(.*\)\"/\1/p"; }
umask 077
secret vault_mqtt_scavhunt > "$R/etc/scavhunt-mqtt-password"
secret vault_mqtt_admin > "$R/etc/admin-mqtt-password"
render_hub() {
    HUB_PASSWORD=$(secret vault_mqtt_roclink_hub) /usr/bin/python3 -c '
import os, sys
sys.stdout.write(open(sys.argv[1]).read().replace("@HUB_PASSWORD@", os.environ["HUB_PASSWORD"]))' "$HERE/etc/hub.toml.in"
}
for f in hub.toml game.toml; do
    if [ -f "$R/etc/$f" ] && [ "$CONFIGS" != --configs ]; then
        echo "   kept $R/etc/$f (--configs to write it again)"
        continue
    fi
    [ -f "$R/etc/$f" ] && cp "$R/etc/$f" "$R/etc/$f.bak"
    if [ $f = hub.toml ]; then render_hub > "$R/etc/$f"; else cp "$HERE/etc/$f" "$R/etc/$f"; fi
    echo "   wrote $R/etc/$f"
done
umask 022

ln -sf "$HERE/roclink" "$HOME/bin/roclink"
echo "== done: roclink restart (or start) to run this version"
