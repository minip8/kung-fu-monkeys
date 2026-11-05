#!/usr/bin/env bash
# Moves a VM to a new release (deploy/README.md "Updating"): pulls the images
# at TAG, waits until the lobby reports no live room (the drain), refreshes
# the systemd units and the Caddyfile from this directory, records TAG in
# /etc/kfm/kfm.env, restarts the lobby (if this VM runs one) and then the
# server pool one instance at a time, so the rest of the pool stays
# registered while each process comes back.
#
#   sudo deploy/vm/update.sh v0.2.0                  # drain up to 15 min, then update
#   sudo deploy/vm/update.sh v0.2.0 --drain 60       # wait at most a minute
#   sudo deploy/vm/update.sh v0.2.0 --no-drain       # restart at once
#
# Restarting a server ends the match it hosts; restarting the lobby closes
# every room (servers re-register when they reconnect or restart). The drain
# polls the lobby's /healthz (locally on a lobby VM, else KFM_LOBBY_URL's
# host over https) every 10 s until "rooms":0 or the timeout, then proceeds
# regardless: a release never waits forever on one long evening. The deploy
# workflow runs this on every tagged release (deploy/README.md).
set -euo pipefail

usage() {
  echo "usage: $0 TAG [--drain SECONDS] [--no-drain]" >&2
  exit 2
}

tag=""
drain=900
while [[ $# -gt 0 ]]; do
  case "$1" in
    --drain)
      [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || usage
      drain="$2"
      shift
      ;;
    --no-drain) drain=0 ;;
    -*) usage ;;
    *)
      [[ -z "$tag" ]] || usage
      tag="$1"
      ;;
  esac
  shift
done
[[ -n "$tag" ]] || usage
[[ $EUID -eq 0 ]] || {
  echo "error: run as root" >&2
  exit 1
}
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
env_file=/etc/kfm/kfm.env
[[ -f $env_file ]] || {
  echo "error: $env_file missing; run install.sh first" >&2
  exit 1
}
repo="$(sed -n 's/^KFM_IMAGE_REPO=//p' "$env_file" | tail -n 1)"
[[ -n $repo ]] || {
  echo "error: KFM_IMAGE_REPO is empty in $env_file" >&2
  exit 1
}

lobby=0
systemctl is-enabled --quiet kfm-lobby.service 2> /dev/null && lobby=1

# Pull first: a missing tag fails here, before anything restarts.
docker pull "$repo/kfm-server:$tag"
[[ $lobby -eq 0 ]] || docker pull "$repo/kfm-lobby:$tag"

# The drain: the lobby's room count, local on a lobby VM, else through the
# lobby's public name (KFM_LOBBY_URL=wss://host or ws://host:port).
healthz=""
if [[ $lobby -eq 1 ]]; then
  healthz="http://127.0.0.1:8080/healthz"
else
  lobby_url="$(sed -n 's/^KFM_LOBBY_URL=//p' "$env_file" | tail -n 1)"
  case "$lobby_url" in
    wss://*) healthz="https://${lobby_url#wss://}/healthz" ;;
    ws://*) healthz="http://${lobby_url#ws://}/healthz" ;;
  esac
fi
rooms() {
  curl -fsS --max-time 5 "$healthz" 2> /dev/null | grep -o '"rooms":[0-9]*' | cut -d: -f2
}
if [[ $drain -gt 0 && -n "$healthz" ]]; then
  deadline=$((SECONDS + drain))
  while :; do
    n="$(rooms || true)"
    if [[ -z "$n" ]]; then
      echo "drain: $healthz unreachable; not waiting"
      break
    fi
    [[ "$n" != 0 ]] || break
    if [[ $SECONDS -ge $deadline ]]; then
      echo "drain: $n room(s) still live after ${drain}s; updating anyway"
      break
    fi
    echo "drain: $n room(s) live; waiting ($((deadline - SECONDS))s left)"
    sleep 10
  done
elif [[ $drain -gt 0 ]]; then
  echo "drain: no lobby address in $env_file; not waiting"
fi

# Refresh the units and the Caddyfile (the workflow ships deploy/vm before
# running this, so a release can change them); install.sh keeps the rest.
install -m 0644 "$here/kfm-server@.service" /etc/systemd/system/kfm-server@.service
if [[ $lobby -eq 1 ]]; then
  install -m 0644 "$here/kfm-lobby.service" /etc/systemd/system/kfm-lobby.service
  if ! cmp -s "$here/Caddyfile" /etc/caddy/Caddyfile; then
    install -m 0644 "$here/Caddyfile" /etc/caddy/Caddyfile
    systemctl reload caddy || systemctl restart caddy
  fi
fi
systemctl daemon-reload

if grep -q '^KFM_IMAGE_TAG=' "$env_file"; then
  sed -i "s|^KFM_IMAGE_TAG=.*|KFM_IMAGE_TAG=$tag|" "$env_file"
else
  echo "KFM_IMAGE_TAG=$tag" >> "$env_file"
fi

if [[ $lobby -eq 1 ]]; then
  systemctl restart kfm-lobby.service
  sleep 2
fi

mapfile -t units < <(systemctl list-units --plain --no-legend --state=active 'kfm-server@*.service' \
  | awk '{print $1}' | sort -V)
for unit in "${units[@]}"; do
  echo "restarting $unit"
  systemctl restart "$unit"
  # Give it time to register before taking the next one down.
  sleep 5
  systemctl is-active --quiet "$unit" || {
    echo "error: $unit did not stay up; journalctl -u $unit" >&2
    exit 1
  }
done

docker image prune -f > /dev/null
echo "kfm: now at $tag (${#units[@]} server(s)$([[ $lobby -eq 1 ]] && echo ', lobby'))"
