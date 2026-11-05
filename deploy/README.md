# Deploying Kung Fu Monkeys (M6a)

The decision record is architecture §13.5, "The M6a deployment (v0.37)"; the lobby's wire
format is `docs/lobby-protocol.md`. Three parts:

| Part | What | Where |
|---|---|---|
| Client page | `kfm_client.html` + `.js` + `.wasm`, `config.js` | GitHub Pages (`https://<owner>.github.io/<repo>/`), published by `.github/workflows/deploy.yml` on a `v*` tag |
| Lobby | `kfm-lobby` (Rust) behind Caddy, the only public TCP endpoint | the EU VM, `ghcr.io/<owner>/kfm-lobby` |
| Game servers | a pool of `kfm_server` processes, one match each, dialling the lobby | one VM per region, `ghcr.io/<owner>/kfm-server` |

Files:

- `Dockerfile.server`, `Dockerfile.lobby`: the two images, built from the repository root.
- `docker-compose.yml`: lobby + two servers on one machine, for development.
- `vm/`: what a VM runs. Docker runs the images with `--network host` under systemd
  (`kfm-lobby.service`, `kfm-server@.service`), configured by `/etc/kfm/kfm.env`
  (`kfm.env.example`); Caddy (`Caddyfile`) on the lobby VM; `install.sh`, `update.sh`.
- `gcp/`: `create_vm.sh` (the EU VM), `add_region.sh` (a server-only VM), `common.sh`.
- `hetzner.md`: the same two VMs on Hetzner Cloud.

## Day one

1. **Names.** The page needs no domain: it is the repository's GitHub Pages site,
   `https://<owner>.github.io/<repo>/` (free for a public repository). The lobby needs a
   hostname that resolves to the EU VM and that Let's Encrypt will certify (the page is
   `https://`, so the browser only accepts `wss://`): either a name on a domain you own
   (`lobby.example.com`, an A record to the VM's IP) or a free dynamic-DNS name such as
   `kfm.duckdns.org` (DuckDNS is on the public suffix list, so Let's Encrypt's rate limits are
   per name, not shared with every other DuckDNS user). `sslip.io`-style names (the IP in the
   name) work with Caddy too but share one rate limit with everyone. Pick the lobby name now:
   it goes into `KFM_LOBBY_DOMAIN`, `KFM_LOBBY_URL` and the DNS record.
2. **GitHub Pages and the repository settings.** In the repository's Settings → Pages set
   *Source* to *GitHub Actions* (once; `deploy.yml` then owns the site). Then:
   ```sh
   gh variable set KFM_LOBBY_URL --body wss://lobby.example.com
   gh variable set KFM_SENTRY_DSN --body '<browser DSN>'   # optional
   ```
   `deploy.yml` writes `config.js` from the two `KFM_` variables:
   `window.KFM_CONFIG = { lobbyUrl: "…", sentryDsn: "…" };`.
3. **First release.** `git tag v0.1.0 && git push origin v0.1.0`. `deploy.yml` pushes
   `ghcr.io/<owner>/kfm-server:v0.1.0` and `kfm-lobby:v0.1.0` (plus `latest`) and publishes
   the page. GHCR packages start private: make both public (package settings), or put a
   `read:packages` token in `KFM_GHCR_USER`/`KFM_GHCR_TOKEN` in `kfm.env`.
4. **GCP project.** Create it, link billing, then
   `gcloud services enable compute.googleapis.com --project my-proj`.
5. **kfm.env.** `cp deploy/vm/kfm.env.example kfm.env` and fill in `KFM_IMAGE_REPO`,
   `KFM_IMAGE_TAG`, `KFM_LOBBY_SECRET` (`openssl rand -hex 32`), `KFM_LOBBY_DOMAIN`. Keep it
   out of git. Passing it to `create_vm.sh` embeds it in the VM's startup-script metadata,
   which anyone with read access to the project can see; for a stricter setup leave
   `--env-file` out and edit `/etc/kfm/kfm.env` over `gcloud compute ssh`.
6. **The VM.**
   ```sh
   deploy/gcp/create_vm.sh --project my-proj --zone europe-west1-b \
     --name kfm-eu-1 --instances 4 --env-file kfm.env
   ```
   It reserves the static IP `kfm-eu-1-ip`, creates the firewall rules `kfm-allow-web`
   (TCP 80/443, tag `kfm-lobby`) and `kfm-allow-ice` (UDP 40000-40099, tag `kfm-server`), and
   an `e2-small` on Ubuntu 24.04 LTS whose startup script runs `install.sh --lobby --servers 4`.
   Progress: `gcloud compute ssh kfm-eu-1 -- sudo journalctl -u google-startup-scripts -f`.
7. **DNS.** An A record `lobby.example.com` → the printed IP. If the zone is on Cloudflare,
   make it *DNS only* (grey cloud): Caddy answers the ACME HTTP challenge itself. Caddy
   retries until the name resolves.
8. **Check.** `curl https://lobby.example.com/healthz` →
   `{"servers":4,"rooms":0,"regions":["eu"]}`. Then open
   `https://<owner>.github.io/<repo>/`, create a room and join it from a second browser with
   `?room=CODE`.

## The ICE port range

The firewall opens UDP 40000-40099 to the servers and the unit passes the same range to every
process as `--ice-ports ${KFM_ICE_PORTS}` (`kfm.env`, default `40000-40099`; keep the two in
step, `create_vm.sh --ice-ports A-B` sets the rule). Every process gets the same range and
libjuice binds a free port inside it per peer connection, so 100 ports cover ten processes of
ten peers.

On Compute Engine the VM's interface holds only its internal address (the external one is
1:1 NAT), so the servers need `KFM_STUN` (default `stun:stun.l.google.com:19302`) to offer
the external address as a server-reflexive candidate.

## Adding a region

```sh
cp kfm.env kfm-us.env   # then: KFM_LOBBY_URL=wss://lobby.example.com, KFM_REGION=us, KFM_NAME_PREFIX=us-1
deploy/gcp/add_region.sh --project my-proj --zone us-east1-b --name kfm-us-1 \
  --instances 4 --env-file kfm-us.env
```

The VM gets tag `kfm-server` only (no inbound TCP): its servers dial the EU lobby over
`wss://`. `/healthz` lists `us` once they register; players pick the region when creating a
room.

## Updating

Tag a release (`v0.2.0`), wait for `deploy.yml`, then on each VM, EU last:

```sh
gcloud compute ssh kfm-us-1 -- sudo /opt/kfm-deploy/vm/update.sh v0.2.0
gcloud compute ssh kfm-eu-1 -- sudo /opt/kfm-deploy/vm/update.sh v0.2.0
```

`update.sh` pulls the tag, writes it into `kfm.env`, restarts the lobby (closing every room)
and then the servers one at a time. A restarted server ends its match, and a client whose
build id or content hash differs from the server's is refused (`mismatch`), so update when
nobody is playing; the page and the servers change together. To change the pool size, re-run
`sudo /opt/kfm-deploy/vm/install.sh [--lobby] --servers N`. To ship changed units or scripts,
copy `deploy/vm/` to `/opt/kfm-deploy/vm/` and re-run `install.sh` (or recreate the VM).

Logs: `journalctl -u kfm-lobby -u 'kfm-server@*' -u caddy`. Each server prints a stats line
every 60 s (`--stats-every 60`).

## What to watch

- **Egress.** The one cost that grows with play: about 100 KB/s per running match, so
  ~0.36 GB per match-hour. Set a billing budget with an alert
  (`gcloud billing budgets create --billing-account … --budget-amount 20USD`) and watch the
  VM's network graph.
- `/healthz`: the number of servers should equal the pool size of every VM. A server that
  drops off restarts under systemd (`Restart=always`) and registers again.
- Sentry (lobby: `SENTRY_DSN` in `kfm.env`; browser: `KFM_SENTRY_DSN`).
- CPU on the VM: a match is a sliver of a core; grow the pool before the machine.

## Switching to Hetzner

When the GCP credits end: `hetzner.md`. Same images, units, Caddyfile and `kfm.env`; only VM
creation and the firewall differ, and the lobby's A record moves.

## Cost

Per region on GCP, without credits, at list prices (check the current ones): an `e2-small`
running all month (about 13 USD), its in-use static external IPv4 (about 3.6 USD), a 20 GB
balanced disk (about 2 USD), and internet egress at roughly 0.08-0.12 USD per GB, which is
about 3-4 US cents per match-hour. GitHub Pages and the Sentry free tier cost nothing. No
spot or preemptible VMs: they are stopped mid-match.
