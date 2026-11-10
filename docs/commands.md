# Kung Fu Monkeys — Commands

Every binary's command line, the content names they take and the release flow. The rules for
writing code are in `CLAUDE.md` and `.claude/rules/`; the design is in `docs/architecture.md`.

Toolchain: CMake ≥ 3.25, Ninja, a C++23 compiler: GCC ≥ 15 or Clang ≥ 19 on Linux (Ubuntu
24.04's GCC 13 and Clang 18 do not work; GCC 14 fails at -Werror), MSVC 2022, Apple Clang 16+,
Emscripten 4+; verified on GCC 15, Clang 21, MSVC 19.51, Apple Clang 21, Emscripten 6. Optional:
vcpkg (`VCPKG_ROOT`; without it every dependency is fetched at configure time at the same pinned
version) and emsdk (`EMSDK`, set by `source ~/emsdk/emsdk_env.sh`) for the `wasm` preset. Build
directories are `build/<preset>`. The client needs no system GL headers (GL is loaded through
the vendored glad loader in `third_party/glad`); building SDL3 on Linux needs the X11/Wayland
dev packages listed in `.github/workflows/ci.yml`.

## Build, test, lint

```sh
cmake --preset dev                      # configure; presets: dev, release, asan, server-headless, wasm
cmake --build --preset dev              # build
ctest --preset dev                      # unit tests + determinism corpus + C++23 feature probe
cmake --workflow --preset dev           # configure + build + test in one go
ctest --preset dev -R kfm_core_tests    # one test executable (kfm_core_tests, kfm_sim_tests, kfm_scenario_tests, determinism_*)
build/dev/tests/unit/kfm_core_tests "[fixed]"      # one Catch2 tag or test name
ctest --preset dev -R kfm_client_tests  # headless client core (clock, session, lobby, options, menus, scoreboard)
ctest --preset dev -R kfm_content_tests # TOML loader, decimal conversion, base kit, arena 0, mode 0 == defaultContent()
ctest --preset dev -R 'kfm_ai|arena'    # engine/ai (observation, reference bot, AgentHost), the wasm fixture suite, kfm_arena
ctest --preset dev -L soak              # the netsim gates (10 clients at 100 ms / 2%, 300 ms / 10%, 500 ms ±30 ms / 5%)
ctest --preset dev -L network           # only the real-socket tests (kfm_net_webrtc_tests, net_probe_loopback); -LE network skips them
cmake --workflow --preset server-headless   # headless Release build of kfm_server + tests (no SDL)
cmake --preset dev -DKFM_WASM_BOTS=OFF  # build without WAMR (reference bots only; a module spec exits 3)
cmake --preset dev -DKFM_WEBRTC=OFF     # build without libdatachannel / datachannel-wasm (no WebRTC transport, no network tests)
tools/ci/check_sim_no_float.sh          # no float/double in engine/sim (and engine/ai)
clang-format -i $(find engine apps tests tools -name '*.cpp' -o -name '*.hpp' | grep -v sin_table)
clang-tidy -p build/dev $(find engine apps tests -name '*.cpp')
python3 tools/codegen/gen_sin_table.py > engine/core/include/kfm/core/sin_table.inc  # trig LUT
cmake --build --preset dev --target regen-replay-corpus   # after an intentional gameplay/content change only
# (re-run `cmake --preset dev` after adding a .kfmr: the gate globs the corpus at configure time)
```

The `wasm` preset runs the same tests under node and builds `kfm_client.html`. CI runs the lint
job, the four native toolchains in Debug and Release (Linux on Ubuntu 26.04, where a
`client_smoke` ctest renders 60 frames of a bot match under xvfb), ASan+UBSan and wasm
(`.github/workflows/ci.yml`). clang-format and clang-tidy never run over `third_party/`.

## Replays

```sh
build/dev/apps/replay/kfm_replay record-empty|record-scripted|info|checksums|verify ...
build/dev/apps/replay/kfm_replay record-scripted out.kfmr --ticks 3600 --players 10 --seed S --input-seed I
build/release/apps/replay/kfm_replay bench --ticks 36000 --players 10   # ticks/s of a scripted match
build/dev/apps/replay/kfm_replay stats --players 4 --matches 20         # feel-tuning stats over scripted matches
```

`record-*`, `bench` and `stats` take `--arena NAME --mode NAME --loadout NAME --powerups
all|none|a,b`; every command takes `--content DIR` (default: the content compiled into the
binary). `info` prints the replay's arena, mode, loadout and both content hashes.

`bench`, `verify`, `stats` and `record-scripted --stats` count the contacts the per-tick
contact list (`kMaxContacts`) dropped (`sim::StepStats`). When any were, they write one line
to stderr after their usual output, `--quiet` included; nothing is printed when none were:

```text
warning: contact list overflow (kMaxContacts 256): 206 contacts dropped over 21 ticks, worst tick 570 with 17
```

`bench` and `stats` name the worst tick's match (`worst match 1 tick 570`: match k is the one
with seed S + k). `verify --fail-on-dropped-contacts` makes any drop fatal: the line starts with
`error:`, the `OK` line is not printed and the exit status is 1, as for a checksum mismatch;
without the flag the exit status is unchanged. The corpus gate does not pass it
(`kfm_corpus_tests` asserts zero drops); `replay_contact_drops` checks the line on a test arena
that overflows (`tests/fixtures/content/crowd.toml`).

## Client

```sh
build/dev/apps/client/kfm_client [--autostart] [--bots N] [--seed S] [--overlay] [--mute] [--volume 0..100]
build/dev/apps/client/kfm_client --content content --arena dojo --mode rounds   # hot-reloads content/ (~1 s) and restarts the match
build/dev/apps/client/kfm_client --arena dojo_powerups --bots 3                 # power-ups: the dojo plus four spawners
build/dev/apps/client/kfm_client --mode grid_rounds --bots 3                    # the grid at 15-tick turns
build/dev/apps/client/kfm_client --bots 3 --bot-difficulty easy|medium|hard [--bot-module bot.wasm]
build/dev/apps/client/kfm_client --exit-after-frames 60 --screenshot out.ppm    # headless render check
build/dev/apps/client/kfm_client --replay tools/replay-corpus/scripted_10p_contest_1800t.kfmr   # the replay viewer
build/dev/apps/client/kfm_client --capsules      # placeholder capsules instead of the skinned test rig
build/dev/apps/render-probe/kfm_render_probe --players 6 --exit-on hit --exit-after-frames 6000 --screenshot hit.ppm
source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm && python3 tools/web/serve.py [--page kfm_render_probe.html]
#   then open http://127.0.0.1:8000/kfm_client.html?autostart&bots=3 (browser build of the same client)
python3 tools/web/check_browser.py 'http://127.0.0.1:8000/kfm_client.html?autostart&bots=3&overlay' --out shots  # Playwright check
```

- The client opens on the main menu unless `--autostart` / `--connect` / `--room` / `--create`
  (browser: `?autostart`, `?connect=`, `?room=`, `?create`). `--screen
  menu|settings|online|local|paused` opens another screen (for screenshots; `paused` is a local
  match under the pause menu).
- Match selection: `--arena NAME --mode NAME --loadout NAME --powerups all|none|a,b`. Settings
  are saved between runs (SDL pref dir / browser localStorage); flags win; `--no-settings`
  ignores the saved file. Other saved settings with a flag: `--nick NAME` (`?nick=`),
  `--mouse-aim on|off` (`?mouse_aim=`), `--bot-difficulty` (`?bot=easy`).
- Browser: `kfm_client.html?arena=pillars&mode=deathmatch&loadout=slam&powerups=shield,big_banana`,
  `?mute&volume=50`. Bot modules are native only (the browser has no wasm runtime for them).
- Content: without `--content`, `content/` next to the executable (copied by the build), else
  the built-in copy. `--no-hot-reload` stops the reload.
- Pacing: where vsync cannot be set (X11 under WSLg) the native client paces itself at the
  display's refresh rate. `--no-frame-limit` runs unpaced: use it for long `--exit-after-frames`
  runs, which otherwise take N/60 seconds.
- Developer keys: F1 overlay (audio, tuning), P pause, O single step, Esc the pause menu.
  Pause, step, time scale and tuning are off online. The controls are in architecture §5.4.
- A moving monkey glides in its idle pose on purpose: the run clip is off behind
  `kMonkeyRunCycle` (`presentation/tuning.hpp`). The rig is generated by the build from
  `tools/assets/gen_test_rig.py`; never commit exported glTF.
- Replay viewer: `--replay SRC`, in the browser `?replay=<url>` (CORS fetch, 16 MiB cap, the
  replay's content hash must be the client's). `--exit-after-frames` exits 0 while playing and
  1 on a bad replay (the error screen is still rendered); seeks step from a keyframe every 600
  ticks. The ctests `client_smoke_replay` and `client_smoke_replay_bad` match the client's
  output (`PASS_REGULAR_EXPRESSION`).
- `kfm_render_probe` renders a scripted match. `--exit-on`: `dash-ring|perfect-catch|auto-catch|
  hit|throw|elimination|catch-window|charge|pit-fall|fall|scatter` (`--fall-at PERCENT`, default
  40, stops once a monkey's pit fall is that far through). `--arena pillars|corridors|island`
  draws an arena with walls / pits / mud. It takes `--content --arena --mode --loadout` too.

## Content names

- Modes (`content/modes.toml`): `rounds` (default), `deathmatch`, `survival`, `team_rounds`,
  `team_deathmatch`, `hill`, `capture`, `grid_rounds`, `grid_rounds_1t` (the grid at 1-tick
  turns, for `kfm_arena`), `contest` (the bot ladder's row: a loader test pins it, so a season's
  change to it is a deliberate edit of both).
- A team mode needs an arena with a spawn group per team, `hill` and `capture` need their
  zones, a grid mode needs an arena with cells (a tilemap): `ContentDB::compatible`. Without
  `--arena` every host picks the first arena that suits; a named arena that does not suit is a
  configuration error (non-zero exit).
- Arenas: `content/arenas.toml` (dojo) and `content/arenas/*.toml`, appended by file name. A
  tilemap arena gives `cell = 1.0` and `map = ["..."]` (`#` wall, `.` floor, `~` pit, `:` mud,
  `0-9` spawns, `a-h` zones) in place of `half_extents`/`walls`/`hazards`/`spawn_points`/`zones`
  (format: `content/arenas/06-hilltop.toml`, architecture §5.10).
- Loadouts (`content/loadouts.toml`): `base` (default), `blink`, `slam`, `parry`, and `grid`,
  `catcher`, `grid_1t`, `catcher_1t` (the grid modes hand those out by role).
- Fog and calls are content: `modes.toml` `vision = { kind = "own", ... }` (bots only; humans
  see everything), `abilities.toml` `kind = "call"` + `range`. What a bot sees and hears:
  `docs/bot-abi.md` §3.1-3.2.

## Server, netsim, online play

```sh
build/dev/apps/server/kfm_server --port 9000 --bots 3 [--token T] [--max-players N] [--exit-after-seconds N] [--stats-every S]
build/dev/apps/client/kfm_client --connect ws://127.0.0.1:9000 [--token T] [--spectate] [--players N] [--stun URL] [--nick NAME]
build/dev/apps/netsim/kfm_netsim --clients 10 --rtt 100 --jitter 10 --loss 2 --seconds 30 [--csv run.csv]   # exits non-zero on any desync
build/dev/apps/netsim/kfm_netsim --clients 6 --mode team_deathmatch --rtt 100 --jitter 10 --loss 2 --seconds 20
build/dev/apps/net-probe/kfm_net_probe --serve 9123          # WebRtcTransport echo server (native)
build/dev/apps/net-probe/kfm_net_probe --connect ws://127.0.0.1:9123 --messages 300   # native end-to-end check
python3 tools/web/check_webrtc.py --messages 200   # browser WebRtcTransport against the native probe server, headless Chromium
cargo run --manifest-path services/lobby/Cargo.toml -- --listen 127.0.0.1:8080 --secret dev   # the lobby service
build/dev/apps/server/kfm_server --lobby ws://127.0.0.1:8080 --lobby-secret dev --region eu --bots 3 [--ice-ports 40000-40099]
build/dev/apps/client/kfm_client --lobby ws://127.0.0.1:8080 --create [--region eu]   # create a room; prints "in room CODE"
build/dev/apps/client/kfm_client --lobby ws://127.0.0.1:8080 --room CODE              # join it (any case)
```

- `kfm_server` runs a room that waits (no ticks) for its host, the first client, to pick the
  match and Start; `--arena/--mode/--loadout/--powerups/--bots` are its defaults. `--auto-start`
  (implied by `--exit-after-matches`) plays back-to-back matches with every bot from the start.
- Server bots: `--bot SPEC` (repeatable, slot order; SPEC = `ref | ref:easy|medium|hard |
  path.wasm`), `--bots N` pads with `ref:hard`, `--bot-delay auto|N` (auto = the humans' lead, 0
  bot vs bot), `--fuel N` (instructions per tick, default 100000), `--memory-on-respawn
  keep|wipe`. `kfm_server` and `kfm_netsim` take `--content DIR` like every other host.
- Lobby mode (`--lobby`): the server dials URL/server and takes no `--port/--bind/--token` (the
  secret also comes from env `KFM_LOBBY_SECRET`); one room per process. `cargo test/fmt/clippy`
  in `services/lobby`; `curl 127.0.0.1:8080/healthz`.
- Client: `-DKFM_LOBBY_URL=...` bakes a default lobby and `-DKFM_PAGE_URL=...` the page that
  Copy link points at; in the browser `kfm_client.html?lobby=ws://127.0.0.1:8080&create` or
  `?room=CODE`, and `config.js` next to the page supplies `lobbyUrl` and `sentryDsn` in
  production. `--verbose` prints the transport's log (ICE, DTLS, the lobby socket, every dropped
  peer).
- A real-link check needs `--exit-after-frames` in the thousands with `--no-frame-limit`:
  unpaced, the client runs hundreds of frames a second.
- The high-RTT table of architecture §6.2: `kfm_netsim --rtt 200|300|500 --jitter 20|30 --loss
  2|5 --seconds 60 --csv`.

## Bots and the arena runner

```sh
build/dev/apps/arena/kfm_arena --bot ref:hard --bot bots/mine.wasm --bots 4 --matches 20 --rotate [--seed S] [--ticks N]
build/dev/apps/arena/kfm_arena --bots 4 --mode grid_rounds_1t --matches 5
build/dev/apps/arena/kfm_arena --mode grid_rounds_1t --team ref:hard,ref:hard,ref:hard,ref:hard,ref:hard --team ref:hard --matches 20 --rotate
tools/bots/build_example.sh      # compile tools/bots/examples/*.c against the C SDK seed with the emsdk clang
tools/bots/build_fixtures.sh     # re-assemble tests/fixtures/bots/*.wat with wat2wasm (the .wasm are checked in)
```

- `--rotate` moves every bot through every slot across the tournament (the sim's spawn order
  favours some slots). One `--team LIST` per team of a team mode (no `--bot/--bots`): the specs
  fill that team's slots in order, the last repeating, and `--rotate` moves entrants across
  sides.
- Also `--arena NAME --mode NAME --loadout NAME --powerups LIST --content DIR`, `--fuel N`,
  `--delay T`, `--memory-on-respawn keep|wipe`, `--replay out.kfmr | --replay-dir DIR`,
  `--result FILE`, `--json`, `--quiet`, `--log`. One result line per match and a summary; exit
  0 ok, 2 args, 3 module refused, 4 I/O.
- The `--json` objects are an append-only contract (`docs/arena-result.md`, normative; a test
  freezes every key, its order and type): a new key is appended to the code, the doc and the
  test, never inserted or renamed. The match object carries `content_hash` and `build_hash`
  (the build id: 0 unless configured with `-DKFM_BUILD_ID=<hex>`, as releases are; peers of
  different build ids are refused and told to reload).

## Releases

`deploy/README.md` is the runbook. A `v*` tag runs `.github/workflows/deploy.yml`: GHCR images,
then `update.sh` on every VM in the `KFM_VMS` variable over IAP (restarting at once), then
Cloudflare Pages (the page is built beside the images by `web-build`; `web` publishes it after
the VMs). The tag is also a GitHub release with the headless `kfm_arena` for
linux-x86_64 (static), macos-arm64 and windows-x86_64 plus `SHA256SUMS`, and the page's site
tarball; the `web` job republishes every release's page under `/<tag>/` (a Pages deploy is a
full snapshot), the viewer of that release's replays. Its last job, `bots-bump`, opens the
engine pin bump in the bots repository as a pull request (as the `kfm-release-bot` GitHub App:
`KFM_RELEASE_BOT_APP_ID` variable, `KFM_RELEASE_BOT_PRIVATE_KEY` secret); merging it and
releasing the ladder stay manual. Local stack: `docker compose -f deploy/docker-compose.yml`.
