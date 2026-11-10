# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Kung Fu Monkeys is an online 2–10 player arena brawler (Boomerang Fu-inspired: monkeys throw
bananas that fly like boomerangs; dash is the base ability; a perfectly timed dash catches a
banana). C++23 engine, deterministic simulation, rollback netcode, the browser as the primary
shipping client, and a public bot API (sandboxed wasm modules, any language).

What is in the tree:

- `engine/core` (fixed point, pools, bit streams, RNG), `engine/sim` (the tick pipeline over
  `ContentDB`), `engine/content` (the TOML loader behind `content/`, embedded into every binary),
  `engine/net` (transports, message layer, `NetServer`, `NetClient`), `engine/ai` (`IAgent`,
  `Observation`, `AgentHost`, the reference bot, `WasmAgent`), `engine/presentation` and
  `engine/platform` (renderer, SDL3, Dear ImGui).
- `apps/`: `client` (`kfm_client`, native and browser), `server` (`kfm_server`), `replay`
  (`kfm_replay`), `arena` (`kfm_arena`, the headless bot match runner), `netsim`, `net-probe`,
  `render-probe`.
- `services/lobby/` (the Rust lobby service: rooms, tokens, signaling, the server registry),
  `deploy/` (images, units, VM scripts; `deploy/README.md` is the runbook), `tools/`, `tests/`.
- The bot SDKs, example bots and the ladder service live in a separate repository,
  `minip8/kung-fu-monkeys-bots`, which pins an engine release in its `engine.lock`.
- Deployed: `kfm.minip8.com` (the page, Cloudflare Pages), `lobby.kfm.minip8.com` (the VM
  `kfm-au-1`, australia-southeast1), `bots.kfm.minip8.com` (the ladder).

Milestones M0 to M6b have landed and M7, the bot ladder, is the current one (M8, a Steam build,
is optional). What each milestone holds and where it stands is in `docs/architecture.md` §11;
what changed in each architecture version is in `docs/architecture-changelog.md`. Neither is
restated here: this file holds only what changes how code is written.

`docs/architecture.md` is the single source of truth for every decision below and is versioned:
its status line carries the version, and `docs/architecture-changelog.md` has one entry per
version, oldest first. When a design decision changes, update the document's body, bump the
version in its status line and append a changelog entry, in the same change. Do not let code
and doc disagree silently. Three more documents are normative for their contracts:
`docs/bot-abi.md` (the bot ABI and Observation v1), `docs/lobby-protocol.md` and
`docs/arena-result.md` (`kfm_arena --json`).

## Commands

Toolchain: CMake ≥ 3.25, Ninja, a C++23 compiler: GCC ≥ 15 or Clang ≥ 19 on Linux (Ubuntu
24.04's GCC 13 and Clang 18 do not work; GCC 14 fails at -Werror), MSVC 2022, Apple Clang 16+,
Emscripten 4+; verified on GCC 15, Clang 21, MSVC 19.51, Apple Clang 21, Emscripten 6. Optional:
vcpkg (`VCPKG_ROOT`; without it every dependency is fetched at configure time at the same pinned
version) and emsdk (`EMSDK`, set by `source ~/emsdk/emsdk_env.sh`) for the `wasm` preset. Build
directories are `build/<preset>`. The client needs no system GL headers (GL is loaded through
the vendored glad loader in `third_party/glad`); building SDL3 on Linux needs the X11/Wayland
dev packages listed in `.github/workflows/ci.yml`.

### Build, test, lint

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

### Replays

```sh
build/dev/apps/replay/kfm_replay record-empty|record-scripted|info|checksums|verify ...
build/dev/apps/replay/kfm_replay record-scripted out.kfmr --ticks 3600 --players 10 --seed S --input-seed I
build/release/apps/replay/kfm_replay bench --ticks 36000 --players 10   # ticks/s of a scripted match
build/dev/apps/replay/kfm_replay stats --players 4 --matches 20         # feel-tuning stats over scripted matches
```

`record-*`, `bench` and `stats` take `--arena NAME --mode NAME --loadout NAME --powerups
all|none|a,b`; every command takes `--content DIR` (default: the content compiled into the
binary). `info` prints the replay's arena, mode, loadout and both content hashes.

### Client

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

### Content names

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

### Server, netsim, online play

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

### Bots and the arena runner

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

### Releases

`deploy/README.md` is the runbook. A `v*` tag runs `.github/workflows/deploy.yml`: GHCR images,
then `update.sh` on every VM in the `KFM_VMS` variable over IAP (restarting at once), then
Cloudflare Pages. The tag is also a GitHub release with the headless `kfm_arena` for
linux-x86_64 (static), macos-arm64 and windows-x86_64 plus `SHA256SUMS`, and the page's site
tarball; the `web` job republishes every release's page under `/<tag>/` (a Pages deploy is a
full snapshot), the viewer of that release's replays. Its last job, `bots-bump`, opens the
engine pin bump in the bots repository as a pull request (as the `kfm-release-bot` GitHub App:
`KFM_RELEASE_BOT_APP_ID` variable, `KFM_RELEASE_BOT_PRIVATE_KEY` secret); merging it and
releasing the ladder stay manual. Local stack: `docker compose -f deploy/docker-compose.yml`.

## Architecture in one screen

Read `docs/architecture.md` §2 (decisions table) first; the rest of this section says which rules
are load-bearing when writing code.

**Layering (§4).** `engine/core` → `engine/sim` → {`engine/net`, `engine/ai`,
`engine/presentation`, `engine/content`} → `apps/`. `sim` depends only on `core` and must never include anything
from `net`, `presentation` or `platform`. `net`, `ai` and `presentation` never depend on each
other. `presentation` reads sim state and never writes it. Enforce with CMake target visibility,
not convention.

**The sim is deterministic by contract (§5.1).** Everything in `engine/sim` must obey:
- Fixed 60 Hz tick (`kTickHz`); no wall clock, no OS randomness. The RNG lives inside `WorldState`.
- Fixed-point math only (`Fixed` = Q16.16 in `int32_t`, `int64_t` intermediates, LUT trig,
  integer sqrt). No `float`/`double` anywhere in `sim`; presentation converts at the boundary.
- `WorldState` is trivially copyable: fixed-capacity `Pool<T, N>`, generational `EntityId`
  handles, no pointers, no heap allocation after init, no iteration over hash containers, no
  exceptions, RTTI or threads.
- Snapshot save/restore for rollback is `memcpy`. Network snapshots, replays and checksums go
  through the visitor serializer (`visit(V&, T&)`), never raw memory (padding bytes).
- Systems are stateless free functions run in the fixed order listed in §5.3. Nothing else
  mutates `WorldState`.

Wasm executables link with `-sSTACK_SIZE=1MB`: a `step()` keeps ~40 KB of per-tick scratch on
the stack and Emscripten's 64 KB default overflows silently in Release.

A replay corpus with per-tick checksums is the CI merge gate across Linux/Windows/macOS/wasm and
Debug/Release. Any sim change that alters checksums is either a bug or an intentional gameplay
change that regenerates the corpus; say which. The corpus is listed in
`tools/replay-corpus/corpus.txt`; `regen-replay-corpus` re-records every entry (replays carry the
content hash) and rewrites the `.checksums`. A new corpus replay needs a manifest line and an
expectation row in `tests/determinism/corpus_events_test.cpp`.

**Content is data (§5.10).** `content/*.toml` (loaded by `engine/content`, toml++) is the source
of truth; `defaultContent()` in `engine/sim/src/content.cpp` is the minimal sim-test fixture
(base kit, dojo, rounds) and the TOML a superset: new arenas, modes, power-ups and abilities are
TOML only, while `kfm_content_tests` checks the base kit, arena 0 and mode 0 agree field by field.
Adding content changes the content hash, so run `regen-replay-corpus` (existing checksums must
not change). New def kind:
struct in `engine/sim/include/kfm/sim/content/`, a `ContentDB` field, its `visit()`, a parser
in `engine/content/src/loader.cpp` (a `Section` call per key; unknown keys are errors) and a
TOML file. Decimals become `Fixed` by exact truncation (never `double`); speeds are per second
in TOML. Arenas and modes are selected per match by `MatchConfig::arena`/`::mode` indices; the
sim reads them as `content.arena`/`content.mode` from the `MatchContent` every system takes.
Append arenas/modes, never reorder (indices are in replays and on the wire).

**Gameplay is added through tables, not special cases (§5.5–5.8).**
- Abilities: `AbilityDef` (content) + POD `AbilityState` per slot + an `AbilityBehavior` function
  table indexed by `AbilityKind`. New ability = new behaviour file, content def, one registry
  line, one scenario test.
- Interactions: `CollisionSystem` emits contacts; `InteractionSystem` resolves them through the
  prioritised rules table keyed on `(kind, tags) × (kind, tags)`. New twist = new rule (+ tag).
  The dash-catch is a rule row, not code inside the dash. A handler returning `false` means "did
  not apply, keep scanning" (a mistimed catch falls through to damage). Rows live in
  `engine/sim/src/rules/table.cpp`.
- Power-ups: `PowerUpDef` grants `StatModifier`/`AbilityOverride`/`Tag`; `ModifierSystem`
  recomputes derived stats each tick.
- `SimEvent`s are the only channel from sim to presentation, scoring and match results.

**Input (§5.4, §5.11).** `PlayerInput` is 5 bytes: `moveDir`, `moveMag`, `aimDir`, `buttons`,
`subTick`. The stick is idle iff `moveMag == 0`; `moveDir` is always an angle. Button edges are detected in `InputSystem`, which is what makes "repeat last input"
a safe remote prediction. `subTick` is the fraction of the tick at which the last button edge
happened; it is used only by time-aware rules (perfect-dash window judged against swept
time-of-impact, tie-breaking), never to move entities within a tick. The same struct is the
bot action space.

**Netcode (§6).** Server-authoritative, input-synchronised, client rollback. Clients send only
inputs; the server steps at 60 Hz, substitutes "repeat last input" for late arrivals, never
waits, and broadcasts authoritative `InputFrame`s. Clients run ahead by a controlled lead,
predict everyone, keep a ring of saved states, and re-simulate on misprediction. Full snapshots
exist only for join/spectate/reconnect/checksum-mismatch. The message layer needs exactly two
lanes from any transport (unreliable-unordered, reliable-ordered); `ITransport` implementations
are `LoopbackTransport` + `NetEmulator` (all netcode tests run here), `WebRtcTransport`
(production, libdatachannel natively and datachannel-wasm in the browser) and an optional
`GnsTransport` for a Steam build. Every transport must pass the same contract test suite.

**Platform seams (§13).** Web first; the native client is the development/profiling build;
Steam is optional. Only `ITransport` and `IPlatformServices` may know which platform they are on.
The browser client is single-threaded by design; do not introduce threads into the client path.

**Presentation and platform (§7).** `presentation` is float code over sim state: sim
`(x, y)` is world `(x, h, −y)`, +y up, sim +x is screen right and sim +y screen up; the camera is
fixed per arena. `kfm_presentation` (RenderState builder, camera, meshes) is headless and unit
tested; `kfm_renderer` (sokol_gfx, GL backends only, hand-written GLSL with a `#version` prefix
per backend) links `kfm::sokol`, never `kfm::platform`. `platform` owns SDL3, the GL context
(4.3 core natively via glad, GLES3/WebGL2 in the browser) and Dear ImGui. Input goes
`SDL_Event` → `platform::InputEvent` → `InputSampler` → `PlayerInput` once per tick, right
before `step()`; the sampler is SDL-free and tested headlessly. Tuning constants live in
`kfm/presentation/tuning.hpp`, never as literals in the renderer.

**Bots (§8).** A bot is a public contract: a sandboxed WebAssembly module behind a four-export
C ABI (`kfm_abi_version`, `kfm_obs_buffer`, `kfm_init`, `kfm_act`), fed a versioned POD
`Observation` and returning a `PlayerInput`, metered in wasm instructions per tick (never wall
clock; over budget or a trap means "repeat last input" for that tick). `IAgent` wraps modules
(`WasmAgent`) and the built-in reference bot alike; agents live outside the sim and server-side
bots write into the authoritative frame directly. Helpers (nearest banana, time to impact) go
in the SDKs, not the host ABI. Human-likeness is a host setting: input delay equal to the
human lead in mixed matches, none bot versus bot. SDKs, example bots and the contest live in the
bots repository; this repo ships no training tooling (no ONNX, no nanobind, no Python env).
The runtime is WAMR 2.4.5's classic interpreter (`kfm::wamr`, FetchContent only, patched at
configure time to read the remaining budget back; `KFM_WASM_BOTS=OFF` builds without it, and
Emscripten never builds it). `kfm_ai_wasm` is a separate target so `kfm_ai` never sees WAMR.
The budgets are 100 k instructions per tick and 10 M for instantiation and `kfm_init`; modules
are at most 4 MiB with 16 MiB of memory. `docs/bot-abi.md` is the normative ABI and Observation
v1 layout, and `tools/bots/sdk-c/kfm_bot.h` is copied byte for byte by the bots repository.
`engine/ai` is fixed-point only (the no-float check covers it): a bot match reproduces bit for
bit on every host. Every host names bots through `BotSpec` and seeds slot s with
`seed ^ (0x9E3779B97F4A7C15 · (s + 1))`; `engine/net`'s only bot hook is
`ServerConfig::botInputs`, and `net` and `ai` never link each other.

## Working conventions specific to this repo

- Strong typedefs for `Tick`, `PlayerId`, `EntityId`; never pass raw integers for these.
- Content (abilities, power-ups, arenas, modes) is data under `content/`, loaded into an immutable
  `ContentDB` and hashed; peers must match `content_hash` and `build_id`.
- Netcode behaviour is tested through the loopback emulator (latency/jitter/loss/reorder), never
  by hand against a live server first.
- Glossary for terms used in code and docs (tick, lead, rollback, authoritative frame, confirmed
  tick, misprediction, sub-tick timestamp, lane): `docs/architecture.md` Appendix B.

## Rules by area

**Sim**

- Teams never enter `WorldState`: a free-for-all config (slot i = team i) must keep every
  checksum. New rows that hurt or push get an Enemy row and an Ally twin needing
  `Tag::FriendlyFire` on the receiving monkey (a test pins the twins); hosts fill
  `MatchConfig::teams` with `sim::teamsFor(mode, present)` (or through `resolveMatchNames`).
  There is no friendly fire unless the mode's `baseline` power-up grants tag `friendly_fire`.
- Objective kinds keep their state in `MatchRules::modeData` (layouts in `src/modes/hill.cpp`,
  `capture.cpp` and `docs/bot-abi.md` §3), never in a new world-state field; a capture flag is a
  `PickupKind::Flag` pickup whose `spawner` byte is its team. The scripted generator's objective
  steering must stay Rng-free and keyed on the mode kind, or every other corpus entry moves.
- Spaces: how monkeys move is `spaceBehaviorFor(mode.space)` (`src/spaces/`), nothing else;
  the continuous row must stay the sim as built bit for bit. Cells come only from
  `kfm/sim/spaces/grid_cells.hpp` (column 0 west, row 0 north, a cell centre = a tilemap spawn
  point). The grid latch reads the frame and the world state only (rollback-exact); contested
  cells are not rule rows. Grid content counts in turns: a grid throw's recovery equals
  `turn_ticks` and roots, a grid dash's active ticks are the slide; a mode's numbers that must
  scale with the turn need their own defs (the `_1t` twins). The contact radius is
  `Stats::radius`, never `PlayerDef::radius`, in sim code.
- Pits: a pit starts a fall (`Tag::Falling`, `Player::fallTicks` from `[player]
  fall_ticks`, `PlayerFell`), never an elimination on the spot, and only once the whole contact
  disc is over the union of the pit boxes (`discOverPits` / `discWithinBoxes` in
  `content/arena.hpp`, exact and integer-only; the bot uses the same function). Row 60 forbids
  `Dashing`, so a dash's reach is how wide a pit it clears. `kGone` in `rules/table.cpp` holds
  `Falling`: a new row that forbids `kGone` is safe against a falling monkey.
- Bananas never disappear: a dead (or newly falling) monkey's bananas in flight are released at
  once (`releaseBananasOf`: dropped as pickups, or gone in a mode whose `death_bananas` is
  vanish), and held ones at a non-pit death in a drop mode, a catch past `ammoMax` and a full
  pickup pool all drop or stack (`rules/bananas.hpp`). A new path that would destroy a banana
  must drop it instead; the exceptions are a vanish mode's death and an ammo power-up's extra
  bananas when it runs out. A grounded banana wholly over a pit sinks instead
  (`BoomerangPhase::Sinking`, never a pickup); a new Boomerang row must name an
  Outbound/Returning phase mask so it never touches a sinking one (a test pins it).
- A new round-ending rule must wait for falls (`anyFalling`), or a doomed last monkey wins; the
  round kinds also end a dry round (no banana left anywhere, `roundRanDry`, `[[mode]]
  dry_round_ticks`), and anything new that makes bananas must count in `bananaLeft`.
- Serialised structs: every `visit()` overload in `kfm/sim/visit.hpp` is followed by a
  `static_assert(fieldCount<T>() == n)`; a new field means the visitor line, the count and a
  mutation in `visit_test.cpp` / `content_test.cpp` (the tests pin their count the same way).
  The scenario tests' hand copies of shipped defs are checked against the TOML by
  `tests/unit/content/mirror_test.cpp`: edit both.
- A new mode hook or test-only mode: `MatchContent::modeBehavior` swaps the kind's
  `ModeBehavior` row in a scenario (`tests/scenario/mode_hooks_scenario.cpp`); hosts never set it.
- Spawn groups fixed the side bias between teams only: the free-for-all spawn-order slot bias
  of §12 is still open (tournaments rotate).

**Bots**

- Fog lives in `engine/ai`, never in the sim: a `vision` change is content hash only, and the
  `all` vision must stay the pre-fog observation byte for byte (a test pins it). Hosts call
  `AgentHost::observe(sink, world, content)` after every `step`, or their bots hear no calls
  (a host that steps the world itself calls `AgentHost::step`, which does fill, step and
  observe; `fill()` asserts the previous step was observed). Agents are made by `ai::BotFactory`
  (`kfm::ai_bots`) and seeded by `ai::agentSeed`, never by a per-host copy.

**Server and lobby**

- The room: room state never enters `WorldState` or `NetServer` (which only reports
  `RoomSettings`/`Start` events and sends the `RoomState` the host sets); policy is
  `apps/server/room.hpp` driven by `ServerHost`. Waiting and Over mean the host does not call
  `NetServer::tick()` at all (the sim leaves `MatchPhase::Lobby` on its first step); clients
  hold (`NetClient::holding()`) while their `RoomState` is not Playing. Tests and `kfm_netsim`
  that drive `NetServer` directly, and `HostConfig{}` (room off), keep the back-to-back flow.
  A full room queues a joiner as a spectator until a bot yields its slot (round end, or a bot
  off the field).
- Lobby protocol changes go to `docs/lobby-protocol.md` first; the Rust suite, `kfm_lobby_tests`
  (label `network`, a fake lobby in-process) and the client's session test all pin it. Every
  envelope is a flat JSON object of strings (nested signaling rides as a string in `msg`) so
  the engine's hand-written decoder reads it. `ServerHost` must stay lobby-agnostic: the wiring
  is `LobbyAdapter` in apps/server. Additions stay backward compatible (a new key beside the
  old ones; a reader treats a missing key as the older lobby's meaning, as a missing `free`
  means "every listed region free"); `/healthz`'s shape is unchanged.
- A running match does not depend on the lobby (a server keeps its peers and room across a
  lost link and claims the room back at `register`; a client with open data channels ignores
  its lobby socket closing). The lobby never emits a frame over 64 KiB; per-address limits
  refuse `limited`; the units pass the lobby secret through the environment.

**Client**

- The UI is Dear ImGui with the game font and the palette of `apps/client/ui_style.hpp` (no
  colour literals in the HUD or screens); screen state lives in headless models in
  `apps/client/core` (`menus.hpp`, `online_menu.hpp`, `scoreboard.hpp`, `hud_view.hpp`), tested
  without a window; hud.cpp / screens.cpp only draw them. `assets/fonts/` holds source assets
  with their licence and is compiled in (never read at run time: the browser has no
  filesystem).
- Clipboard, sharing, saved preferences, the page URL and the replay download go through
  `IPlatformServices` (one `PlatformHooks` value built in
  `apps/client/client_platform_native.cpp` or `client_platform_web.cpp`), never SDL or JS
  directly: `main.cpp` has no `EM_JS`.
- What is highlighted follows the last menu device (`core/menu_highlight.hpp`): mouse mode
  highlights only the hovered item, keys/pad mode the focus, and the first key press from mouse
  mode only reveals it. Draw menu buttons and rows through `ui::bigButton` / `ui::showsFocus` /
  `ui::pointerTakesFocus`, never from `focus()` alone.
- A frame in which a new ImGui window measures itself is not presented
  (`debugUiMeasuring()`); a screen's panel card drawn outside its window must not be relied on
  in that frame.
- The between-rounds scoreboard shows in RoundEnded and at match end only, never over the next
  countdown; a round's pause is 180 ticks (`round_end_ticks`).
- The mouse aims from the local monkey (the predicted one online) through `feedMouseAim` in
  `apps/client/core/mouse_aim.hpp`; clicks over UI windows stay the UI's.

## Lessons from CI (keep a local pass a CI pass)

- Fast iteration: `cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKFM_HEADLESS=ON
  -DKFM_WEBRTC=OFF -DKFM_USE_VCPKG=OFF` builds everything but the client and the WebRTC
  transport in minutes (no SDL3/ImGui/sokol/libdatachannel); `ctest --test-dir build/headless
  -LE 'soak|network'`. Use the full `dev` preset before pushing anything that touches
  `apps/client`, and the `server-headless` preset for the soak and real-socket gates.
- CI's lint job pins clang-format 23.1.3 and clang-tidy 23.1.4 (PyPI wheels: `pip install
  clang-format==23.1.3 clang-tidy==23.1.4`); run those exact versions locally, over the whole
  tree the way `.github/workflows/ci.yml` does, and count both `error` and `warning` lines.
  The versions are recorded in architecture §9; bump both together.
- MSVC `/W4 /WX` differs from GCC: a `return` after Catch2's `FAIL(...)` is unreachable code
  (C4702), so write `if (!opt) FAIL(...); return opt.value_or(...)` rather than `{ FAIL(); return
  {}; }`; `size_t` passed to a `uint32_t` parameter warns (C4267) even when the value is a
  constant GCC can prove fits. clang-tidy's `bugprone-unchecked-optional-access` does not treat
  `REQUIRE(opt)` as a guard; use `if (!opt) { FAIL(); return; }` in a `void` test or `value_or`.
- GCC 15 at -O3 flags copy-assigning a `vector<shared_ptr>` as a null dereference (`-Werror` on
  the Release legs): move instead.
- Third-party sources fetched into `build/<preset>/_deps` may ship their own `.clang-tidy`;
  clang-tidy reads the nearest config for every file a diagnostic lands in, headers included,
  so `third_party/wamr/CMakeLists.txt` overwrites WAMR's with one that disables every check.

## Git workflow

**When to commit.** Commit as you go, without being asked: at the end of each logical unit of
work, as soon as it is complete and (once a build exists) configures, builds and passes tests. A
logical unit is a system, a rule, a transport, a content def with its test, a doc decision. Do not
mix a refactor with a behaviour change. Do not leave finished work uncommitted at the end of a
turn; if something is deliberately left uncommitted (broken, half-done, exploratory), say so.
`main` stays green. Work on a branch (`feat/<topic>`, `fix/<topic>`, `docs/<topic>`), never
directly on `main`; create the branch before the first commit of a task. A design decision, the
`docs/architecture.md` update that records it and its `docs/architecture-changelog.md` entry go
in the same commit.

**Pushing and pull requests.** Push the working branch to `origin` whenever it is green locally
(configures, builds, passes `ctest`, lint clean), and open a pull request against `main` with
`gh pr create` once the task is complete; do not wait to be asked. The PR body summarises what
landed, the decisions made (with the architecture version they were recorded in), how it was
verified (toolchains, corpus, bench numbers) and any follow-ups, and ends with the attribution
line. Never push to `main` directly and never force-push a branch that has an open PR; if history
must be rewritten before the PR exists, say so in the turn. Merging the PR is the user's call:
CI must be green on every leg (the native matrix, ASan+UBSan and wasm) first.

**Message format.** Conventional Commits with the module as scope:

```
<type>(<scope>): <imperative subject, ≤ 72 chars>

<why, not what; the diff shows what>
<for sim changes: "Replay checksums: unchanged" or "Replay checksums: changed, corpus regenerated because ...">
```

Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`, `ci`, `chore`.
Scopes: `core`, `sim`, `net`, `ai`, `presentation`, `platform`, `content`, `apps`, `docs`,
`ci`. Omit the scope only for repo-wide changes.

Examples:

```
feat(sim): add PerfectDash × Boomerang catch rule

Judged against swept time-of-impact and the press sub-tick, per architecture §5.11.
Replay checksums: changed, corpus regenerated because the catch window now starts at press time.
```

```
docs: flip platform strategy to web-first (architecture v0.4)
```

**Never commit** exported glTF or other generated assets, build directories, vcpkg artifacts, replay dumps from
desync investigations, or anything under `/tmp`-style scratch paths. Regenerable assets are built
by the pipeline, not checked in; the source models are.
