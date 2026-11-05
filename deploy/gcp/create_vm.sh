#!/usr/bin/env bash
# The first VM (architecture §13.5, deploy/README.md "Day one"): the EU VM that
# runs Caddy, the lobby and a pool of game servers. Reserves a static external
# IP, opens TCP 80/443 to tag kfm-lobby and the ICE UDP range to tag
# kfm-server, and creates an e2-small on Ubuntu 24.04 LTS (the host only runs
# containers) whose startup script runs deploy/vm/install.sh --lobby.
#
#   deploy/gcp/create_vm.sh --project my-proj --zone europe-west1-b \
#       --name kfm-eu-1 --instances 4 --env-file kfm.env
#
# Idempotent up to the instance: existing addresses and firewall rules are
# reused; an existing VM of that name makes `instances create` fail.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"
name="kfm-eu-1"

usage() {
  echo "usage: $0 --project ID --zone ZONE [options]" >&2
  usage_common
}
parse_common "$@"

reserve_address
create_firewall kfm-allow-web kfm-lobby tcp:80,tcp:443 \
  "Caddy in front of the Kung Fu Monkeys lobby (HTTP for ACME, HTTPS/WSS)"
create_firewall kfm-allow-ice kfm-server "udp:$ice_ports" \
  "ICE UDP range of kfm_server (WebRTC data channels)"

startup="$(mktemp)"
trap 'rm -f "$startup"' EXIT
write_startup_script "$startup" --lobby --servers "$instances"

"${gc[@]}" compute instances create "$name" \
  --zone "$zone" \
  --machine-type "$machine_type" \
  --provisioning-model STANDARD \
  --image-family ubuntu-2404-lts-amd64 --image-project ubuntu-os-cloud \
  --boot-disk-size 20GB --boot-disk-type pd-balanced \
  --network "$network" --address "$address" \
  --tags kfm-lobby,kfm-server \
  --metadata-from-file startup-script="$startup"

cat << EOF

Created $name in $zone at $address.
Next (deploy/README.md):
  1. DNS: an A record for the lobby name (KFM_LOBBY_DOMAIN) -> $address
  2. kfm.env: $([[ -n "$env_file" ]] && echo "installed from $env_file" || echo "gcloud compute ssh $name --zone $zone, edit /etc/kfm/kfm.env, then
     sudo /opt/kfm-deploy/vm/install.sh --lobby --servers $instances")
  3. curl https://<lobby name>/healthz
  Startup log: gcloud compute ssh $name --zone $zone -- sudo journalctl -u google-startup-scripts
EOF
