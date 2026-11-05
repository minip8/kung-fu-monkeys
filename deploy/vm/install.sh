#!/usr/bin/env bash
# Sets up a Kung Fu Monkeys VM (architecture §13.5, deploy/README.md):
# Docker from Docker's apt repository, Caddy from its apt repository (lobby VM
# only), the systemd units, and N kfm-server@ instances running the GHCR
# images at KFM_IMAGE_TAG. Idempotent: safe to re-run (the GCP startup script
# runs it on every boot), and re-running with a smaller N stops the extra
# instances.
#
#   sudo deploy/vm/install.sh --lobby --servers 4   # the EU VM: lobby + Caddy + 4 servers
#   sudo deploy/vm/install.sh --servers 4           # a server-only VM
#
# Configuration is /etc/kfm/kfm.env (kfm.env.example). When it is missing the
# example is installed and nothing is started: fill it in and run again.
set -euo pipefail

usage() {
  echo "usage: $0 [--lobby] [--servers N]" >&2
  exit 2
}

lobby=0
servers=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lobby) lobby=1 ;;
    --servers)
      [[ $# -ge 2 ]] || usage
      servers="$2"
      shift
      ;;
    -h | --help) usage ;;
    *) usage ;;
  esac
  shift
done
[[ "$servers" =~ ^[0-9]+$ ]] || usage
[[ $EUID -eq 0 ]] || {
  echo "error: run as root" >&2
  exit 1
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
env_file=/etc/kfm/kfm.env
export DEBIAN_FRONTEND=noninteractive

# shellcheck source=/dev/null
. /etc/os-release # ID (ubuntu|debian), VERSION_CODENAME

apt_updated=0
apt_install() {
  if [[ $apt_updated -eq 0 ]]; then
    apt-get update -q
    apt_updated=1
  fi
  apt-get install -y -q --no-install-recommends "$@"
}

# --- Docker Engine (https://docs.docker.com/engine/install/) ------------------
if ! command -v docker > /dev/null; then
  apt_install ca-certificates curl gnupg
  install -m 0755 -d /etc/apt/keyrings
  curl -fsSL "https://download.docker.com/linux/${ID}/gpg" -o /etc/apt/keyrings/docker.asc
  chmod a+r /etc/apt/keyrings/docker.asc
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc]" \
    "https://download.docker.com/linux/${ID} ${VERSION_CODENAME} stable" \
    > /etc/apt/sources.list.d/docker.list
  apt_updated=0
  apt_install docker-ce docker-ce-cli containerd.io
fi
systemctl enable --now docker

# --- Caddy (https://caddyserver.com/docs/install#debian-ubuntu-raspbian) ------
if [[ $lobby -eq 1 ]] && ! command -v caddy > /dev/null; then
  apt_install ca-certificates curl gnupg
  curl -fsSL 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
    | gpg --dearmor --yes -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
  curl -fsSL 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
    > /etc/apt/sources.list.d/caddy-stable.list
  chmod a+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg /etc/apt/sources.list.d/caddy-stable.list
  apt_updated=0
  apt_install caddy
fi

# --- Units and configuration -------------------------------------------------
install -m 0644 "$here/kfm-server@.service" /etc/systemd/system/kfm-server@.service
if [[ $lobby -eq 1 ]]; then
  install -m 0644 "$here/kfm-lobby.service" /etc/systemd/system/kfm-lobby.service
  install -m 0644 "$here/Caddyfile" /etc/caddy/Caddyfile
  install -d /etc/systemd/system/caddy.service.d
  printf '[Service]\nEnvironmentFile=/etc/kfm/kfm.env\n' > /etc/systemd/system/caddy.service.d/kfm.conf
fi
systemctl daemon-reload

install -d -m 0755 /etc/kfm
if [[ ! -f $env_file ]]; then
  install -m 0600 "$here/kfm.env.example" "$env_file"
  echo "Installed $env_file from the example: fill in KFM_LOBBY_SECRET (and the rest), then run" >&2
  echo "  sudo $0 $([[ $lobby -eq 1 ]] && echo '--lobby ')--servers $servers" >&2
  exit 0
fi
chmod 0600 "$env_file"

# kfm.env is systemd EnvironmentFile syntax, not shell: read keys, never source it.
env_get() { sed -n "s/^$1=//p" "$env_file" | tail -n 1; }
KFM_IMAGE_REPO="$(env_get KFM_IMAGE_REPO)"
KFM_IMAGE_TAG="$(env_get KFM_IMAGE_TAG)"
KFM_GHCR_USER="$(env_get KFM_GHCR_USER)"
KFM_GHCR_TOKEN="$(env_get KFM_GHCR_TOKEN)"
for key in KFM_IMAGE_REPO KFM_IMAGE_TAG KFM_LOBBY_SECRET KFM_LOBBY_URL KFM_REGION KFM_NAME_PREFIX KFM_BOTS KFM_STUN KFM_ICE_PORTS; do
  if [[ -z "$(env_get "$key")" ]]; then
    echo "error: $key is empty in $env_file" >&2
    exit 1
  fi
done

# --- Images -------------------------------------------------------------------
if [[ -n "$KFM_GHCR_TOKEN" ]]; then
  echo "$KFM_GHCR_TOKEN" | docker login ghcr.io -u "${KFM_GHCR_USER:?set with KFM_GHCR_TOKEN}" --password-stdin
fi
docker pull "$KFM_IMAGE_REPO/kfm-server:$KFM_IMAGE_TAG"
[[ $lobby -eq 0 ]] || docker pull "$KFM_IMAGE_REPO/kfm-lobby:$KFM_IMAGE_TAG"

# --- Start ---------------------------------------------------------------------
if [[ $lobby -eq 1 ]]; then
  systemctl enable --now kfm-lobby.service
  systemctl enable caddy
  systemctl restart caddy
fi
for ((i = 1; i <= servers; i++)); do
  systemctl enable --now "kfm-server@$i.service"
done
# Instances beyond N from an earlier run (running or enabled).
shopt -s nullglob
for unit in $(systemctl list-units --all --plain --no-legend 'kfm-server@*.service' | awk '{print $1}') \
  /etc/systemd/system/multi-user.target.wants/kfm-server@*.service; do
  n="${unit##*/kfm-server@}"
  n="${n#kfm-server@}"
  n="${n%.service}"
  if [[ "$n" =~ ^[0-9]+$ ]] && ((n > servers)); then
    systemctl disable --now "kfm-server@$n.service"
  fi
done

echo "kfm: $servers server(s)$([[ $lobby -eq 1 ]] && echo ', lobby and Caddy') running $KFM_IMAGE_REPO/*:$KFM_IMAGE_TAG"
