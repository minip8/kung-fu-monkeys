# Deploying Kung Fu Monkeys (M6a)

The decision record is architecture §13.5, "The M6a deployment (v0.37)"; the lobby's wire
format is `docs/lobby-protocol.md`. Three parts, plus what a release publishes for the bot
ladder (M7, architecture §8 "The ladder"):

| Part | What | Where |
|---|---|---|
| Client page | `kfm_client.html` + `.js` + `.wasm`, `config.js`, `favicon.svg`, `_headers` | Cloudflare Pages (`play.<domain>`), published by `.github/workflows/deploy.yml` on a `v*` tag: the newest at `/`, every release at `/<tag>/` |
| Lobby | `kfm-lobby` (Rust) behind Caddy, the only public TCP endpoint | the EU VM, `ghcr.io/<owner>/kfm-lobby` |
| Game servers | a pool of `kfm_server` processes, one match each, dialling the lobby | one VM per region, `ghcr.io/<owner>/kfm-server` |
| Release assets | the headless `kfm_arena` per OS, `SHA256SUMS`, the page as `kfm_client-site-<tag>.tar.gz` | the tag's GitHub release, created by `deploy.yml` ("Releases and versioned pages" below) |

Files:

- `Dockerfile.server`, `Dockerfile.lobby`: the two images, built from the repository root.
- `docker-compose.yml`: lobby + two servers on one machine, for development.
- `vm/`: what a VM runs. Docker runs the images with `--network host` under systemd
  (`kfm-lobby.service`, `kfm-server@.service`), configured by `/etc/kfm/kfm.env`
  (`kfm.env.example`); Caddy (`Caddyfile`) on the lobby VM; `install.sh`, `update.sh`.
- `gcp/`: `create_vm.sh` (the EU VM), `add_region.sh` (a server-only VM), `common.sh`.
- `hetzner.md`: the same two VMs on Hetzner Cloud.

## Day one

1. **Names.** A domain whose DNS is on Cloudflare, with two names: the page
   (`play.example.com`, the Cloudflare Pages project's custom domain, which the dashboard
   wires into DNS itself) and the lobby (`lobby.example.com`, an A record to the EU VM, set to
   *DNS only*, grey cloud, so Caddy obtains the certificate and the WebSocket reaches the VM
   directly). The lobby name goes into `KFM_LOBBY_DOMAIN`, `KFM_LOBBY_URL` and the DNS record.
   Without a domain, a free dynamic-DNS name such as `kfm.duckdns.org` serves the lobby.
2. **Cloudflare Pages and the repository settings.** In the Cloudflare dashboard, Workers &
   Pages → Create → Pages → *Upload assets* (a direct-upload project; do not connect it to the
   repository, `deploy.yml` uploads the built site), name it, then in the project's *Custom
   domains* tab add `play.example.com`. Create an API token (My Profile → API Tokens) with the
   permission *Cloudflare Pages: Edit* on the account, and read the account id from the
   dashboard's sidebar. Then, in the GitHub repository:
   ```sh
   gh secret set CLOUDFLARE_API_TOKEN
   gh secret set CLOUDFLARE_ACCOUNT_ID
   gh variable set CF_PAGES_PROJECT --body kfm
   gh variable set KFM_LOBBY_URL --body wss://lobby.example.com
   gh variable set KFM_SENTRY_DSN --body '<browser DSN>'   # optional
   ```
   `deploy.yml` writes `config.js` from the two `KFM_` variables:
   `window.KFM_CONFIG = { lobbyUrl: "…", sentryDsn: "…" };`.
3. **First release.** `git tag v0.1.0 && git push origin v0.1.0`. `deploy.yml` pushes
   `ghcr.io/<owner>/kfm-server:v0.1.0` and `kfm-lobby:v0.1.0` (plus `latest`), publishes
   the page and creates the GitHub release `v0.1.0` with its assets. GHCR packages start private: make both public (package settings), or put a
   `read:packages` token in `KFM_GHCR_USER`/`KFM_GHCR_TOKEN` in `kfm.env`.
4. **GCP project.** Create it, link billing, then
   `gcloud services enable compute.googleapis.com --project my-proj`.
5. **kfm.env.** `cp deploy/vm/kfm.env.example kfm.env` and fill in `KFM_IMAGE_REPO`,
   `KFM_IMAGE_TAG`, `KFM_LOBBY_SECRET` (`openssl rand -hex 32`), `KFM_LOBBY_DOMAIN`. Keep it
   out of git. Passing it to `create_vm.sh` embeds it in the VM's startup-script metadata,
   which anyone with read access to the project can see; for a stricter setup leave
   `--env-file` out and edit `/etc/kfm/kfm.env` over `gcloud compute ssh`. On the VM the
   units hand `KFM_LOBBY_SECRET` to the lobby and server containers through the environment
   (`docker run -e KFM_LOBBY_SECRET`), never a command-line flag, which any local user could
   read in `ps` or `/proc` and anyone with the Docker socket in `docker inspect`.
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
8. **Automatic updates.** `deploy/gcp/setup_deploy_identity.sh --project my-proj --repo
   owner/name`, then the `gh variable set` lines it prints ("Updating" below). From then on
   every `v*` tag updates the VMs before the page goes live.
9. **Check.** `curl https://lobby.example.com/healthz` →
   `{"servers":4,"rooms":0,"regions":["eu"]}`. Then open
   `https://play.example.com/`, create a room and join it from a second browser with
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

Tag a release (`v0.2.0`) and the deploy workflow does the rest: it pushes the images, then
its `vms` job ships `deploy/vm/` to every VM in the repository variable `KFM_VMS` over an IAP
SSH tunnel and runs `update.sh v0.2.0` on each (in the listed order: lobby VM last), and only
then publishes the page, so a browser never meets a server of another build. `update.sh`
pulls the tag, refreshes the units and the Caddyfile from the shipped copy, writes the tag
into `kfm.env`, restarts the lobby and then the servers one at a time, at once: a match in
progress ends with its server's restart. The lobby's own restart ends no match (lobby protocol
v1.3: servers keep their connected players, re-register within seconds and claim their rooms
back, `docs/lobby-protocol.md` §5); joins and creates fail while it is down. The first release
with v1.3 is the exception: its lobby restart happens while the old servers still run, and a
server older than v1.3 closes every peer with its lobby link, so those matches end as before. Watch it under Actions → Deploy; the job's log shows every
restart.

One-time setup (architecture §13.5): `deploy/gcp/setup_deploy_identity.sh --project my-proj
--repo owner/name` creates the service account, the Workload Identity Federation pool and
provider for this repository, turns OS Login on for the project and opens port 22 to IAP's
range only, then prints the `gh variable set` lines (`GCP_PROJECT_ID`,
`GCP_WORKLOAD_IDENTITY_PROVIDER`, `GCP_DEPLOY_SERVICE_ACCOUNT`, `KFM_VMS` as
`"kfm-us-1:us-east1-b kfm-eu-1:europe-west1-b"`). Without `KFM_VMS` the job is skipped and
the page is published straight after the images. A manual `workflow_dispatch` run never
touches the VMs (its images are `sha-` tagged).

By hand, for one VM or when the workflow cannot reach it:

```sh
gcloud compute ssh kfm-eu-1 --zone europe-west1-b --tunnel-through-iap -- \
  sudo /opt/kfm-deploy/vm/update.sh v0.2.0 [--drain SECONDS]
```

A restarted server ends its match and a client whose build id or content hash differs from
the server's is refused (`mismatch`). `--drain SECONDS` first polls the lobby's `/healthz`
until `"rooms":0` (or the timeout, then goes ahead regardless) for an update that should wait
for an evening's last room to close. To change the pool size, re-run `sudo /opt/kfm-deploy/vm/install.sh
[--lobby] --servers N`. The VM's startup script re-extracts the `deploy/vm/` embedded at
creation on every boot, so units or a Caddyfile changed by a release hold until the next
reboot; to make them permanent, recreate the VM (or update its startup-script metadata).

Logs: `journalctl -u kfm-lobby -u 'kfm-server@*' -u caddy`. Each server prints a stats line
every 60 s (`--stats-every 60`).

## Releases and versioned pages

From M7 step 1 (architecture §8 "The ladder", v0.59) every `v*` tag is also a GitHub release.
The workflow's `release` job creates it (with generated notes; a re-run finds the existing one)
and two jobs upload to it with `--clobber`, so re-running a deploy replaces its own assets:

| Asset | From | What |
|---|---|---|
| `kfm_arena-<tag>-linux-x86_64.tar.gz` | `arena` job, ubuntu-26.04, GCC | fully static (`-static`): runs on any x86_64 Linux, no glibc floor |
| `kfm_arena-<tag>-macos-arm64.tar.gz` | `arena` job, macos-latest, Apple Clang | needs the macOS the runner built for (its `minos` is in README.txt); not notarised |
| `kfm_arena-<tag>-windows-x86_64.zip` | `arena` job, windows-latest, MSVC | static CRT: no Visual C++ redistributable |
| `SHA256SUMS` | `arena-sums` job | `sha256sum` format over the three archives (`sha256sum -c SHA256SUMS`) |
| `kfm_client-site-<tag>.tar.gz` | `web` job | this release's page: `index.html`, `kfm_client.html`, `config.js`, `favicon.svg`, `assets/<hash>/` (no `_headers`) |

Each `kfm_arena` archive holds `kfm_arena` (`kfm_arena.exe`), `README.txt` (the tag, the
commit, the usage line, what the host needs) and `LICENSE` once the repository has one, all at
the archive's root; the content is compiled in. It is the headless Release build
(`server-headless` preset, FetchContent, no WebRTC) with the wasm bot runtime, smoke-run before
upload with four reference bots and with `tests/fixtures/bots/echo.wasm`. The ladder's CLI
(`kfm test`) and scheduler download it by these names. The `arena` jobs run beside the images
and the VMs and never wait for them; a manual run builds the same archives (named
`sha-<short>` for the tag) as workflow artifacts and creates no release.

**The bots repository's pin.** The ladder runs the engine release named in the bots
repository's `engine.lock`, baked into its service image, so an engine fix reaches the ladder
only when that pin moves and the bots repository is released. The workflow's last job,
`bots-bump`, opens the first half as a pull request: on a `vX.Y.Z` tag, once `SHA256SUMS` is
on the release, it checks out the bots repository, runs its `tools/bump_engine.sh <tag>` (the
tag's commit into `engine.lock`, the SDK header copy, the lines of its docs that name the pin)
and pushes `chore/engine-<tag>`. The pull request is the bots CI's to check (it plays the
examples with the new `kfm_arena`) and yours to merge: when the script's output in the pull
request says the header changed, bring `sdk/cpp/kfm_bot.hpp` and the examples along on that
branch first. Releasing the ladder afterwards is a tag of the bots repository, by hand.

The job authenticates as the `kfm-release-bot` GitHub App (a commit pushed with `GITHUB_TOKEN`
would start no CI there). Set it up once: create the App on the account that owns both
repositories (webhook off; repository permissions Contents and Pull requests, read and write;
installable only on this account), install it on `kung-fu-monkeys-bots` only, and store its
App ID as this repository's variable `KFM_RELEASE_BOT_APP_ID` and a generated private key (the
`.pem`'s text) as the secret `KFM_RELEASE_BOT_PRIVATE_KEY`. Without the variable the job is
skipped and the bump is by hand (`tools/bump_engine.sh` in a clone of the bots repository). A
re-run of the workflow updates the branch and keeps the open pull request.

**Versioned pages.** A replay is viewed in the client of its own release (§8 "Replays"), so the
site serves every release under its tag, `https://play.example.com/v0.60/` (keep the trailing
slash: the page's references are relative), beside the root, which serves the release being
deployed. A Pages deployment is a full snapshot of `site/`, so the `web` job rebuilds every
versioned path each time: it uploads this release's site asset first, then unpacks the site
asset of every earlier non-draft `v*` release under `site/<tag>/`, adds this release under
`site/<tag>/`, and writes `_headers` with `/:version/...` rules beside the root's (assets
immutable, the page, `config.js` and `favicon.svg` revalidated). A release without a site asset
(every tag before the first release made by this workflow, v0.1.0 to v0.4.0) is skipped with a
notice. Each versioned copy keeps the `config.js` of its release, the lobby URL and Sentry DSN
at that release: an old page that opens Play online reaches the current lobby, whose servers
refuse its build id, so versioned pages are for viewing replays and playing locally. All
versions share one origin, so settings saved in `localStorage` are shared too (an older client
warns about a key it does not know and carries on). A manual run publishes the branch's build at the
root plus the earlier releases' paths and uploads no release asset.

**Re-publishing an old tag's page.** When a release lacks its site asset (it failed, or it
predates this workflow), build it from that tag and attach it; the next deploy serves it:

```sh
git worktree add ../kfm-v0.58 v0.58 && cd ../kfm-v0.58
source ~/emsdk/emsdk_env.sh && cmake --preset wasm && cmake --build --preset wasm --target kfm_client
# then run the shell of the "Assemble this release's page" step of main's deploy.yml here,
# with that release's KFM_LOBBY_URL and KFM_SENTRY_DSN in the environment: it writes release-site/
tar -czf kfm_client-site-v0.58.tar.gz --owner=0 --group=0 -C release-site .
gh release view v0.58 > /dev/null 2>&1 || gh release create v0.58 --verify-tag --title v0.58 --notes 'Page only.'
gh release upload v0.58 kfm_client-site-v0.58.tar.gz --clobber
```

To serve it at once rather than at the next tag, re-run only the `web` job of the newest tag's
Deploy run (Actions → the run → `web` → Re-run this job: the images and VMs are not touched).
A manual run of the workflow from a branch other than `main` is a preview of that branch; one
from `main` publishes to production, as before. Deleting a release's site asset (or the release) drops `/<tag>/` at the next deploy.

**Retention.** One release adds six files (about 3.4 MB, the `.wasm` being 3.2 MB of it, far
under Pages' 25 MiB per file). Pages allows 20,000 files per site on the free plan, so about
3,000 releases fit; the web job prints the count and warns past 19,000. The practical cost is
the web job downloading every earlier site asset (about 1.2 MB each) and wrangler hashing the
files, which it uploads only when new (a re-served release costs no upload). At a release a
week that is years away; when it matters, prune by deleting old releases' site assets (or
restrict the loop to the last N tags) and decide which replay links may break.

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
about 3-4 US cents per match-hour. Cloudflare Pages and the Sentry free tier cost nothing. No
spot or preemptible VMs: they are stopped mid-match.
