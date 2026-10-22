#!/bin/bash
# The rocbotnet workspace: where each repository lives, and git across all of them.
#
#   ./workspace.sh status          each repository's branch, sync state and changed files
#   ./workspace.sh pull | push     git pull --ff-only / git push, in each one
#   ./workspace.sh clone BASE      clone the missing ones from BASE/<name>.git
#                                  (e.g. git@github.com:you)
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REPOS=(
    protocol/roclink-rfc
    rfgateways/roclink-gateway
    hub/roclink-hub
    hub/roclink-scavhunt
    rfidproxies/rfidprox
    scavboxes/roclink-scavbox-wifi
    scavboxes/roclink-scavbox-lora
    infra/roclink-bench
)
case ${1:-status} in
    status)
        for r in "${REPOS[@]}"; do
            if [ ! -d "$ROOT/$r/.git" ]; then printf '%-34s missing\n' "$r"; continue; fi
            printf '%-34s %s, %s changed\n' "$r" "$(git -C "$ROOT/$r" status -sb | head -1 | cut -c4-)" \
                "$(git -C "$ROOT/$r" status --short | wc -l | tr -d ' ')"
        done ;;
    pull|push)
        for r in "${REPOS[@]}"; do
            echo "== $r"
            if [ "$1" = pull ]; then git -C "$ROOT/$r" pull --ff-only; else git -C "$ROOT/$r" push; fi
        done ;;
    clone)
        [ -n "${2:-}" ] || { echo "clone BASE, e.g. clone git@github.com:you"; exit 1; }
        for r in "${REPOS[@]}"; do
            [ -d "$ROOT/$r" ] || git clone "$2/$(basename "$r").git" "$ROOT/$r"
        done ;;
    *) sed -n '2,9p' "$0"; exit 1 ;;
esac
