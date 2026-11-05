# The same deployment on Hetzner Cloud

Architecture §13.5 (v0.37): Compute Engine first, Hetzner Cloud when the credits end. Nothing
in `deploy/vm/` is provider-specific, so the switch is two `hcloud` VMs running the same
startup script and a DNS change; `update.sh` works as before. Install the CLI
(`hcloud context create kfm` with a project API token) and an SSH key
(`hcloud ssh-key create --name me --public-key-from-file ~/.ssh/id_ed25519.pub`).

Differences from GCP:

- The server's interface holds its public IPv4 (no 1:1 NAT), so ICE host candidates are
  already reachable; `KFM_STUN` is harmless and may be emptied.
- Firewalls attach to servers by name (or label), not network tags.
- A primary IP created on its own outlives the server, like GCP's reserved address.
- Traffic is included up to a quota per server (20 TB in the EU locations, less in the US and
  Singapore ones at the time of writing), so egress stops being the cost that grows with play.
- User data runs once, on first boot (cloud-init); the units it installs persist. Re-run
  `install.sh` by hand to change the pool size.

## Firewalls

```sh
hcloud firewall create --name kfm-lobby
hcloud firewall add-rule kfm-lobby --direction in --protocol tcp --port 80  --source-ips 0.0.0.0/0 --source-ips ::/0
hcloud firewall add-rule kfm-lobby --direction in --protocol tcp --port 443 --source-ips 0.0.0.0/0 --source-ips ::/0

hcloud firewall create --name kfm-server
hcloud firewall add-rule kfm-server --direction in --protocol tcp --port 22 --source-ips 0.0.0.0/0 --source-ips ::/0
hcloud firewall add-rule kfm-server --direction in --protocol udp --port 40000-40099 --source-ips 0.0.0.0/0 --source-ips ::/0
```

The UDP range is the one `KFM_SERVER_EXTRA_ARGS` gives `kfm_server` (see README.md, "The ICE
port range"); until the server takes it, open `32768-60999` (Linux's ephemeral range) instead.

## The startup script

The GCP scripts embed `deploy/vm/` and the env file into one shell script; reuse it as user
data:

```sh
bash -c '. deploy/gcp/common.sh; env_file=kfm.env;    write_startup_script eu.sh --lobby --servers 4'
bash -c '. deploy/gcp/common.sh; env_file=kfm-us.env; write_startup_script us.sh --servers 4'
```

## The EU VM (lobby, Caddy, servers)

```sh
hcloud primary-ip create --name kfm-eu-1-ip --type ipv4 --datacenter fsn1-dc14
hcloud server create --name kfm-eu-1 --type cx22 --image ubuntu-24.04 --location fsn1 \
  --ssh-key me --primary-ipv4 kfm-eu-1-ip --firewall kfm-lobby --firewall kfm-server \
  --user-data-from-file eu.sh
hcloud primary-ip describe kfm-eu-1-ip -o format='{{.IP}}'
```

## A server-only VM (another region)

```sh
hcloud primary-ip create --name kfm-us-1-ip --type ipv4 --datacenter ash-dc1
hcloud server create --name kfm-us-1 --type cpx11 --image ubuntu-24.04 --location ash \
  --ssh-key me --primary-ipv4 kfm-us-1-ip --firewall kfm-server \
  --user-data-from-file us.sh
```

`kfm-us.env` carries `KFM_LOBBY_URL=wss://<lobby name>`, the EU lobby's secret,
`KFM_REGION=us` and `KFM_NAME_PREFIX=us-1`. Server types differ per location (`cx22` is EU only;
`hcloud server-type list`).

## Switching over

1. Create both VMs above with the same `kfm.env` values (same secret, same image tag).
2. Lower the lobby record's TTL a day ahead, then point the A record at the new EU IP; Caddy
   gets a certificate once DNS resolves (`journalctl -u caddy`).
3. Server-only VMs follow automatically: they dial the lobby by name.
4. Check `https://<lobby name>/healthz`, then delete the GCP VMs and release their addresses
   (`gcloud compute instances delete`, `gcloud compute addresses delete`).
