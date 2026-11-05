#!/usr/bin/env bash
# A server-only VM in another region (architecture §13.5, deploy/README.md
# "Adding a region"): game servers that dial the EU lobby, no Caddy, no lobby,
# no inbound TCP. Reserves a static external IP, makes sure the ICE UDP rule
# for tag kfm-server exists, and creates the VM with only that tag.
#
#   deploy/gcp/add_region.sh --project my-proj --zone us-east1-b \
#       --name kfm-us-1 --instances 4 --env-file kfm-us.env
#
# The env file needs KFM_LOBBY_URL=wss://<lobby name> (the EU lobby through
# Caddy), the EU lobby's KFM_LOBBY_SECRET, and this VM's KFM_REGION and
# KFM_NAME_PREFIX (e.g. us, us-1).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$here/common.sh"
name="kfm-region-1"

usage() {
  echo "usage: $0 --project ID --zone ZONE --env-file FILE [--name NAME] [options]" >&2
  usage_common
}
parse_common "$@"
[[ -n "$env_file" ]] || usage
lobby_url="$(sed -n 's/^KFM_LOBBY_URL=//p' "$env_file" | tail -n 1)"
if [[ "$lobby_url" != wss://* ]]; then
  echo "error: KFM_LOBBY_URL in $env_file must be the EU lobby's wss:// URL (is '$lobby_url')" >&2
  exit 2
fi

reserve_address
create_firewall kfm-allow-ice kfm-server "udp:$ice_ports" \
  "ICE UDP range of kfm_server (WebRTC data channels)"

startup="$(mktemp)"
trap 'rm -f "$startup"' EXIT
write_startup_script "$startup" --servers "$instances"

"${gc[@]}" compute instances create "$name" \
  --zone "$zone" \
  --machine-type "$machine_type" \
  --provisioning-model STANDARD \
  --image-family ubuntu-2404-lts-amd64 --image-project ubuntu-os-cloud \
  --boot-disk-size 20GB --boot-disk-type pd-balanced \
  --network "$network" --address "$address" \
  --tags kfm-server \
  --metadata-from-file startup-script="$startup"

cat << EOF

Created $name in $zone at $address, registering with $lobby_url.
Check: curl https://<lobby name>/healthz lists its region once the servers are up.
Startup log: gcloud compute ssh $name --zone $zone -- sudo journalctl -u google-startup-scripts
EOF
