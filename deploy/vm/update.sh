#!/usr/bin/env bash
# Moves a VM to a new release (deploy/README.md "Updating"): pulls the images
# at TAG, records TAG in /etc/kfm/kfm.env, restarts the lobby (if this VM runs
# one) and then the server pool one instance at a time, so the rest of the
# pool stays registered while each process comes back.
#
#   sudo deploy/vm/update.sh v0.2.0
#
# Restarting a server ends the match it hosts; restarting the lobby closes
# every room (servers re-register when they reconnect or restart). Update when
# nobody is playing.
set -euo pipefail

[[ $# -eq 1 ]] || {
  echo "usage: $0 TAG" >&2
  exit 2
}
[[ $EUID -eq 0 ]] || {
  echo "error: run as root" >&2
  exit 1
}
tag="$1"
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
