# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

Kung Fu Monkeys is an online 2–10 player arena brawler (Boomerang Fu-inspired: monkeys throw
bananas that fly like boomerangs; dash is the base ability; a perfectly timed dash catches a
banana). C++23 engine, deterministic simulation, rollback netcode, browser as the primary shipping
client, with a public bot API (sandboxed wasm modules, any language) planned.

Milestones M0 (skeleton), M1 (headless sim), M2 (playable local client), M3 (netcode) and M4
(content) have landed: build system, `engine/core`, the
full `engine/sim` tick pipeline (modes, Dash, Throw, Blink, Slam, Parry, banana flight and
return, swept collision against entities and arena statics, the interaction rules table,
power-ups and modifiers, lifecycle) over `ContentDB`, a scripted input generator, `kfm_replay`
(record, verify, bench) and the determinism gate over a corpus of empty and scripted replays;
`engine/content` (the TOML loader behind `content/`, embedded into every binary, hot reload);
`engine/platform` (SDL3 window + GL context, audio device, sokol_gfx, Dear ImGui, the SDL-free
input sampler), `engine/presentation` (float math, fixed arena camera, RenderState builder,
event-driven effects and procedural sound, procedural meshes, skinning, the sokol renderer) and
`apps/client` (local play with the lobby settings panel, bots, HUD, F1 overlay, the browser
page); `engine/net` (`ITransport`, loopback + emulator, message layer, `NetServer`, `NetClient`
with rollback and clock sync, `WebRtcTransport`), `apps/server`, `apps/netsim`, `apps/net-probe`
and the client's `--connect` mode; `engine/ai` (`IAgent`, `Observation` v1 and its fixed-point
builder, `AgentHost`, the reference bot, `BotSpec`, `WasmAgent` over WAMR with instruction
metering), `apps/arena` (`kfm_arena`, the headless match runner), bots on the server and in the
client lobby, and the C SDK seed with an example bot under `tools/bots/` (M5, architecture
v0.30; the SDKs themselves live in a separate bots repository). M5b landed in v0.35 (teams and
relations, the five mode seams, the loadout axis, zones and tilemap arenas, the `grid` space and
the squad contest shape with fog and calls; designed in architecture v0.29, §5.3, §5.10, §5.12,
§8; the human playtest of `hill` and a 15-tick grid round is pending): step 1, mode foundations (32 mode slots, the `[[loadout]]` axis picked by
`MatchConfig::loadout`, the `onRoundStart`/`onPickupTaken` hooks and `MatchRules::modeData`), landed
in v0.31; step 2, teams (`MatchConfig::teams` → `MatchContent::relation`, the rules table's
Ally rows behind `Tag::FriendlyFire`, the mode baseline power-up and roles, arena spawn groups
and `ContentDB::compatible`, `team_rounds`/`team_deathmatch` on the `standoff` arena), landed in
v0.32; step 3, objectives (arena zones and `zoneOccupants()`, tilemap arenas, `hill` and
`capture` with the flag pickup and `Tag::Carrying`, the `hilltop` and `outposts` arenas), landed
in v0.33; step 4, the `grid` space (the `SpaceBehavior` table behind `InputSystem`'s latch and
`MovementSystem`'s move, cell commitments per turn of `turnTicks` with contested cells, the
dash as a commitment of `dashCells`, `AbilityDef::rootPhases`, the radius stat, the catcher
role, `grid_rounds`/`grid_rounds_1t` on the `courtyard` arena), landed in v0.34; step 5, the
squad contest (the `call` ability and `MonkeyCalled`, `ModeDef::vision` and fog as a projection
in `buildObservation` (`kfm/ai/vision.hpp`), the turn window, heard calls and per-instance
lifecycle in `AgentHost`, the filled Observation v1 fields, `kfm_arena --team` and
`--memory-on-respawn`, a grid- and fog-aware reference bot), landed in v0.35. M6 was split in
v0.36 into M6a (ship on the web: hosting, the lobby service, regional servers) and the bot
ladder (accounts and teams, source-only C++ submissions built in a sandbox, ratings, replays, a
Rust CLI; the service lives in the bots repository, architecture §8 "The ladder"), which v0.40
renumbered to M7 behind a new M6b, "Play with friends" (the online room and the UI pass; Steam
is M8). M6a's shape
was decided in v0.37 and its code landed in v0.38 (§13.5, `docs/lobby-protocol.md`): the Rust
lobby service in `services/lobby/` owns rooms, tokens, signaling and the server registry;
servers dial it (`kfm_server --lobby`, `LobbyLink`, external signaling on `WebRtcTransport`);
clients create or join rooms (`--lobby/--room/--create`, `?room=`); one `kfm_server` per match
under systemd; `deploy/` holds the images, units and VM scripts; Google Cloud Compute Engine
then Hetzner, Cloudflare Pages for the page (v0.39), Caddy, Sentry. Deployed: `kfm.minip8.com` (the page) and
`lobby.kfm.minip8.com` (the VM `kfm-au-1`, australia-southeast1, `v0.1.0`). M6b, "Play with
friends", has its code landed in v0.41-v0.44 (lobby protocol v1.1's region availability; the
online room's Waiting/Playing/Over with the host's settings, protocol 6 and nicknames; the main
menu, waiting screen, scoreboards and match-over; bots yielding their slots to joining humans,
Reconnect from the UI through lobby protocol v1.2's rejoin, the jitter-aware lead band checked to
500 ms RTT); the friends playtest against its criterion was played and M6b landed in v0.58. M6b step 1, lobby protocol v1.1 (the `regions` reply's `list` is every
registered region and `free` the ones with a free server; the client's Create room row marks a
busy region "(no server)"), landed in v0.41. M6b step 2, the online room (protocol 6:
`RoomSettings`/`RoomState`/`Start` and a nickname in `Hello`; `apps/server/room.hpp` beside
`ServerHost`: Waiting and Over never tick the `NetServer`, the host is the first then the
longest-present human slot, its Start places the bots and restarts the match with its
settings; `NetClient` holds its timeline outside Playing; a functional waiting / match-over
screen and nicknames on the HUD; `kfm_server` runs the room unless `--auto-start`), landed in
v0.42. M6b step 3, the client's front end (the game font Lilita One under `assets/fonts/`,
compiled in by `cmake/EmbedFile.cmake`; one palette and ImGui theme in `apps/client/ui_style.hpp`;
a main menu with Play online / Play local / Settings; the Play online screen that replaced the
lobby panel's Online section; the room's waiting screen with the code large, Copy link and Share
through `IPlatformServices`; a scoreboard view model shown between rounds and on a match-over
screen with Play again), landed in v0.43. Keyboard-and-mouse controls landed in v0.45 (§5.4: the
primary keyboard profile is keyboard + mouse; the cursor aims from the local monkey, the
predicted one online, through `feedMouseAim` in `apps/client/core/mouse_aim.hpp`; left click
throws, right click dashes; clicks over UI windows stay the UI's).
`docs/architecture.md` is the
single source of truth for every decision below and is versioned (status line + changelog at the
top). When a design decision changes, update the doc in the same change, bump its version and add
a changelog line. Do not let code and doc disagree silently.

## Commands

Toolchain: CMake ≥ 3.25, Ninja, a C++23 compiler: GCC ≥ 15 or Clang ≥ 19 on Linux (Ubuntu
24.04's GCC 13 and Clang 18 do not work; GCC 14 fails at -Werror, v0.62), MSVC 2022, Apple Clang 16+, Emscripten 4+; verified on
GCC 15, Clang 21, MSVC 19.51, Apple Clang 21, Emscripten 6. Optional: vcpkg (`VCPKG_ROOT`; without it
every dependency is fetched at configure time at the same pinned version) and emsdk (`EMSDK`, set by
`source ~/emsdk/emsdk_env.sh`) for the `wasm` preset. Build directories are `build/<preset>`.
The client needs no system GL headers (GL is loaded through the vendored glad loader in
`third_party/glad`); building SDL3 on Linux needs the X11/Wayland dev packages listed in
`.github/workflows/ci.yml`.

```sh
cmake --preset dev                      # configure; presets: dev, release, asan, server-headless, wasm
cmake --build --preset dev              # build
ctest --preset dev                      # unit tests + determinism corpus + C++23 feature probe
cmake --workflow --preset dev           # configure + build + test in one go
ctest --preset dev -R kfm_core_tests    # one test executable (kfm_core_tests, kfm_sim_tests, kfm_scenario_tests, determinism_*)
build/dev/tests/unit/kfm_core_tests "[fixed]"      # one Catch2 tag or test name
cmake --build --preset dev --target regen-replay-corpus   # after an intentional gameplay/content change only
# (re-run `cmake --preset dev` after adding a .kfmr: the gate globs the corpus at configure time)
tools/ci/check_sim_no_float.sh          # no float/double in engine/sim
clang-format -i $(find engine apps tests tools -name '*.cpp' -o -name '*.hpp' | grep -v sin_table)
clang-tidy -p build/dev $(find engine apps tests -name '*.cpp')
build/dev/apps/replay/kfm_replay record-empty|record-scripted|info|checksums|verify ...  # replay tool
build/dev/apps/replay/kfm_replay record-scripted out.kfmr --ticks 3600 --players 10 --seed S --input-seed I
#   record-*/bench/stats take --arena NAME --mode NAME --loadout NAME; every command takes --content DIR (default:
#   the content compiled into the binary); `info` prints the replay's arena/mode/loadout and both content hashes
build/release/apps/replay/kfm_replay bench --ticks 36000 --players 10   # ticks/s of a scripted match
python3 tools/codegen/gen_sin_table.py > engine/core/include/kfm/core/sin_table.inc  # trig LUT
build/dev/apps/client/kfm_client [--autostart] [--bots N] [--seed S] [--overlay] [--mute] [--volume 0..100]   # playable client
#   v0.54: where vsync cannot be set (X11 under WSLg) the native client paces itself at the display's refresh rate;
#   --no-frame-limit runs unpaced (use it for long --exit-after-frames runs, which otherwise take N/60 seconds).
#   A moving monkey glides in its idle pose: the run clip is off behind kMonkeyRunCycle (presentation/tuning.hpp)
build/dev/apps/client/kfm_client --content content --arena dojo --mode rounds   # edit content/*.toml while it
#   runs: local play hot-reloads the directory (~1 s) and restarts the match (--no-hot-reload to stop it).
#   Without --content: content/ next to the executable (copied by the build), else the built-in copy.
#   modes (content/modes.toml): rounds (default), deathmatch (respawns, score, clock), survival (rounds on a clock),
#   team_rounds and team_deathmatch (2 teams, balanced over the joined slots; v0.32). A team mode needs an arena with a
#   spawn group per team (spawn_groups, ContentDB::compatible): without --arena every host picks the first that suits
#   (standoff), a named arena that does not suit is a configuration error (non-zero exit); no friendly fire unless the mode's
#   `baseline` power-up grants tag friendly_fire. v0.33: hill (hold zone 0 alone, a point per hill_ticks; on hilltop)
#   and capture (carry the enemy flag into your zone while yours is home; a zone per team: outposts), both 2 teams
#   with deathmatch's clock and respawns; compatible() also needs their zones. v0.34: grid_rounds (team rounds in the
#   grid space: one of eight neighbouring cells per 15-tick turn, a dash 2 cells, a throw costs the next turn, one
#   catcher per team by role, the grid_base baseline: radius 0.35, bananas 0.15, HP 2) and grid_rounds_1t (the same
#   at 1-tick turns, for kfm_arena); a grid mode needs an arena with cells (a tilemap) and opens on its own
#   (modes.toml `arena`: courtyard). v0.60: contest, the bot ladder's row (M7), today grid_rounds_1t renamed; a test
#   pins the equality, so a season's change to the row is a deliberate edit of both
#   arenas: content/arenas.toml (dojo) and content/arenas/*.toml, appended by file name; a tilemap arena gives
#   cell = 1.0 and map = ["..."] (# wall . floor ~ pit : mud 0-9 spawns a-h zones) in place of half_extents/walls/
#   hazards/spawn_points/zones (format: content/arenas/06-hilltop.toml, architecture §5.10)
#   loadouts (content/loadouts.toml, --loadout NAME): base (default, [player] abilities), blink, slam, parry;
#   v0.34 grid, catcher, grid_1t, catcher_1t (the grid modes hand them out by role)
#   v0.43 front end: the client opens on the main menu (Play online, Play local, Settings, Quit natively) unless
#   --autostart / --connect / --room / --create (or ?autostart, ?connect=, ?room=, ?create); --screen
#   menu|settings|online|local|paused opens another screen (screenshots; paused: a local match under the pause menu). Menus: arrows/WASD, any pad's d-pad/stick, the
#   mouse; Enter/Space or a pad's South/Start selects, Esc or a pad's East/Back goes back (the menu quits natively).
#   Settings: name (--nick, ?nick=, saved `nick`), volume, sound, bot difficulty, arena theme, mouse aim
#   (v0.55: --mouse-aim on|off, ?mouse_aim=, saved `mouse_aim`; off: the movement aims as before v0.45, clicks
#   still throw/dash). Play online: region (with "(no
#   server)"), Create room, room code + Join. Esc from the lobby or any online screen returns to the menu
#   lobby: Enter/Start begins; press throw/dash (or click) on a free device to join; -/= bots. F1 overlay, Esc menu
#   keys: left side = keyboard + mouse (WASD move, the mouse aims once it moves unless Settings > Mouse aim is off, left click/J/Space throw, right
#   click/K/LShift dash, L/E ability; v0.45), right side = arrows ./RCtrl throw //RShift dash ;/' ability
#   v0.48 charged throw: Throw is hold and release; the hold scales the banana's speed and so its reach (x0.6
#   for a tap to x1.5 after 40 ticks: abilities.toml charge_min/charge_max/charge_ticks); the aim bar grows,
#   the monkey winds up (v0.53: the rig's `charge` clip, scrubbed by the charge) and strains at full charge.
#   Grid throws do not charge
#   v0.46 pause menu: Esc (or a bound pad's Start) in a running match: locally the match pauses (Resume, Back to
#   lobby, Main menu); online it runs on and your slots send idle input (Resume, Leave room: one Esc never leaves);
#   Esc / a pad's East, Back or Start resumes. P pause and O single step stay developer keys (no menu)
#   match over: Play again / Back to lobby (Left/Right + Enter, or the mouse); online the host's Play again / Leave
#   lobby settings panel: arrows/WASD or a joined pad's d-pad/stick (Up/Down choose, Left/Right change) or the mouse:
#   arena (with preview; only arenas that suit the mode), mode, teams (read-only, from the mode), loadout, power-up
#   toggles, bots, volume, mute; saved between runs (SDL pref dir /
#   browser localStorage), flags win: --arena NAME --mode NAME --loadout NAME --powerups all|none|a,b ;
#   --no-settings ignores the saved file
#   browser: kfm_client.html?arena=pillars&mode=deathmatch&loadout=slam&powerups=shield,big_banana (?mode=team_rounds)
#   audio: procedural sounds, silent without a device (F1 > Audio: mute, volume); browser: ?mute&volume=50
ctest --preset dev -R kfm_client_tests                 # headless client core (clock, session, lobby, options, menus, scoreboard)
ctest --preset dev -R kfm_content_tests                # TOML loader, decimal conversion, base kit, arena 0, mode 0 == defaultContent()
build/dev/apps/client/kfm_client --exit-after-frames 60 --screenshot out.ppm   # headless render check
build/dev/apps/render-probe/kfm_render_probe --players 6 --exit-on hit --exit-after-frames 6000 --screenshot hit.ppm
#   renders a scripted match; --exit-on: dash-ring|perfect-catch|auto-catch|hit|throw|elimination|catch-window|charge|
#   pit-fall|fall|scatter (v0.57: --fall-at PERCENT, default 40, stops once a monkey's pit fall is that far
#   through; scatter: a few frames after a dead monkey's held bananas scattered)
#   --arena pillars|corridors|island draws an arena with walls / pits / mud (content/arenas/*.toml)
build/dev/apps/client/kfm_client --capsules                # placeholder capsules instead of the skinned test rig
#   (the rig is generated by the build from tools/assets/gen_test_rig.py; never commit exported glTF)
build/dev/apps/replay/kfm_replay stats --players 4 --matches 20   # feel-tuning stats over scripted matches
build/dev/apps/client/kfm_client --arena dojo_powerups --bots 3   # power-ups: the dojo plus four spawners (content/powerups.toml)
source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm && python3 tools/web/serve.py [--page kfm_render_probe.html]
#   then open http://127.0.0.1:8000/kfm_client.html?autostart&bots=3 (browser build of the same client)
python3 tools/web/check_browser.py 'http://127.0.0.1:8000/kfm_client.html?autostart&bots=3&overlay' --out shots  # Playwright check
build/dev/apps/server/kfm_server --port 9000 --bots 3 [--token T] [--max-players N] [--exit-after-seconds N] [--stats-every S]   # dedicated server
#   v0.42: a room that waits (no ticks) for its host (the first client) to pick the match and Start; --arena/--mode/
#   --loadout/--powerups/--bots are its defaults; --auto-start (implied by --exit-after-matches) keeps the M3 back-to-back
#   matches with every bot from the start
#   bots: --bot SPEC (repeatable, slot order; SPEC = ref | ref:easy|medium|hard | path.wasm), --bots N pads with
#   ref:hard, --bot-delay auto|N (auto = the humans' lead, 0 bot vs bot), --fuel N (instructions per tick, default 100000)
#   also --content DIR --arena NAME --mode NAME --loadout NAME (kfm_netsim and kfm_render_probe take the same four)
#   and --powerups all|none|name,name (the power-up set of every match; kfm_replay record-*/bench/stats too)
build/dev/apps/netsim/kfm_netsim --clients 10 --rtt 100 --jitter 10 --loss 2 --seconds 30 [--csv run.csv]   # soak over the emulator; exits non-zero on any desync
build/dev/apps/netsim/kfm_netsim --clients 6 --mode team_deathmatch --rtt 100 --jitter 10 --loss 2 --seconds 20   # a team mode (teams ride in MatchConfig, protocol 5)
ctest --preset dev -L soak                     # the netsim gates (10 clients at 100 ms / 2%, 300 ms / 10%, v0.44 500 ms ±30 ms / 5%)
#   the high-RTT table of architecture §6.2 (v0.44): kfm_netsim --rtt 200|300|500 --jitter 20|30 --loss 2|5 --seconds 60 --csv
build/dev/apps/arena/kfm_arena --bot ref:hard --bot bots/mine.wasm --bots 4 --matches 20 --rotate [--seed S] [--ticks N]   # headless bot matches
#   --rotate moves every bot through every slot across the tournament (the sim's spawn order favours some slots)
#   [--arena NAME --mode NAME --loadout NAME --powerups LIST --content DIR] [--fuel N] [--delay T] [--replay out.kfmr | --replay-dir DIR]
#   [--result FILE] [--json] [--quiet] [--log]: one result line per match (places, scores, kills, deaths, acts,
#   substitutions, per-module traps/over-budget/fuel) and a summary; exit 0 ok, 2 args, 3 module refused, 4 I/O
#   v0.60: the --json objects are an append-only contract (docs/arena-result.md, normative; a test freezes every key,
#   its order and type: a new key is appended to the code, the doc and the test, never inserted or renamed); the match
#   object carries content_hash and build_hash (zero until a build stamps one)
build/dev/apps/arena/kfm_arena --bots 4 --mode grid_rounds_1t --matches 5   # v0.34: the grid at 1-tick turns (courtyard)
build/dev/apps/arena/kfm_arena --mode grid_rounds_1t --team ref:hard,ref:hard,ref:hard,ref:hard,ref:hard --team ref:hard --matches 20 --rotate
#   v0.35: one --team LIST per team of a team mode (no --bot/--bots): the specs fill that team's slots in order, the
#   last repeating; --rotate moves entrants across sides; team segments, winner_team and per-entrant totals in the
#   result; --memory-on-respawn keep|wipe (kfm_server too): an eliminated bot is paused, wipe re-inits it on respawn
#   fog and calls: content/modes.toml `vision = { kind = "own", ... }` (bots only; humans see everything),
#   abilities.toml `kind = "call"` + `range`; what a bot sees and hears: docs/bot-abi.md §3.1-3.2
build/dev/apps/client/kfm_client --mode grid_rounds --bots 3   # the grid at 15-tick turns: the stick picks a neighbour cell per turn
ctest --preset dev -R 'kfm_ai|arena'           # engine/ai (observation, reference bot, AgentHost), the wasm fixture suite, kfm_arena
build/dev/apps/client/kfm_client --bots 3 --bot-difficulty easy|medium|hard [--bot-module bot.wasm]   # lobby bots (saved: bot_difficulty)
#   browser: kfm_client.html?autostart&bots=3&bot=easy (modules are native only: the browser has no runtime yet)
tools/bots/build_example.sh                    # compile tools/bots/examples/*.c against the C SDK seed with the emsdk clang
tools/bots/build_fixtures.sh                   # re-assemble tests/fixtures/bots/*.wat with wat2wasm (the .wasm are checked in)
cmake --preset dev -DKFM_WASM_BOTS=OFF         # build without WAMR (reference bots only; a module spec exits 3)
cmake --workflow --preset server-headless      # headless Release build of kfm_server + tests (no SDL)
build/dev/apps/client/kfm_client --connect ws://127.0.0.1:9000 [--token T] [--spectate] [--players N] [--stun URL] [--nick NAME]   # join a server
#   --nick (?nick=, saved as nick=; default a random monkey name picked once): the name the room and the HUD show
#   browser: kfm_client.html?connect=ws://host:9000&token=T ; Esc leaves (in a running match: the pause menu's
#   Leave room); pause/step/time scale/tuning are off online
cargo run --manifest-path services/lobby/Cargo.toml -- --listen 127.0.0.1:8080 --secret dev   # the lobby service (M6a,
#   docs/lobby-protocol.md); cargo test/fmt/clippy there; curl 127.0.0.1:8080/healthz
build/dev/apps/server/kfm_server --lobby ws://127.0.0.1:8080 --lobby-secret dev --region eu --bots 3 [--ice-ports 40000-40099]
#   lobby mode: dials URL/server, no --port/--bind/--token (secret also from env KFM_LOBBY_SECRET); one room per process
build/dev/apps/client/kfm_client --lobby ws://127.0.0.1:8080 --create [--region eu]   # create a room; prints "in room CODE"
build/dev/apps/client/kfm_client --lobby ws://127.0.0.1:8080 --room CODE              # join it (any case); -DKFM_LOBBY_URL=...
#   bakes a default; browser: kfm_client.html?lobby=ws://127.0.0.1:8080&create or ?room=CODE (config.js next to the page
#   supplies lobbyUrl and sentryDsn in production); the menu's Play online screen does the same; Esc leaves
#   (in a running match: the pause menu's Leave room);
#   waiting screen: the code large, Copy link (the page's ?room=CODE link; natively -DKFM_PAGE_URL=... or the code)
#   and Share on touch browsers; the host's rows take arrows/WASD/pads, Enter starts
#   --verbose prints the transport's log (ICE, DTLS, the lobby socket, every dropped peer). v0.44: a lost link offers
#   Reconnect (the same server with the same token, or a lobby rejoin of the room: lobby protocol v1.2 `join` + `token`);
#   a full room queues a joiner as a spectator until a bot yields its slot (round end, or a bot off the field). A real-link check needs
#   --exit-after-frames in the thousands with --no-frame-limit: unpaced, the client runs hundreds of frames a second
deploy/README.md                                 # the runbook: images (deploy/Dockerfile.*), VM units, gcloud scripts, release
#   workflow (.github/workflows/deploy.yml on a v* tag: GHCR images, then update.sh on every VM in the KFM_VMS
#   variable over IAP, restarting at once (v0.47; deploy/gcp/setup_deploy_identity.sh once), then Cloudflare Pages);
#   v0.60: a v* tag is also a GitHub release with the headless kfm_arena for linux-x86_64 (static), macos-arm64 and
#   windows-x86_64 plus SHA256SUMS, and the page's site tarball; the web job republishes every release's page under
#   /<tag>/ (a Pages deploy is a full snapshot), the viewer of that release's replays (architecture v0.59)
#   docker compose -f deploy/docker-compose.yml
ctest --preset dev -L network                  # only the real-socket tests (kfm_net_webrtc_tests, net_probe_loopback); -LE network skips them
build/dev/apps/net-probe/kfm_net_probe --serve 9123          # WebRtcTransport echo server (native)
build/dev/apps/net-probe/kfm_net_probe --connect ws://127.0.0.1:9123 --messages 300   # native end-to-end check
python3 tools/web/check_webrtc.py --messages 200   # browser WebRtcTransport (datachannel-wasm) against the native probe server, headless Chromium
cmake --preset dev -DKFM_WEBRTC=OFF             # build without libdatachannel / datachannel-wasm (no WebRTC transport, no network tests)
```

The `wasm` preset runs the same tests under node and builds `kfm_client.html`. CI runs the lint
job, the four native toolchains in Debug and Release (Linux on Ubuntu 26.04, where a
`client_smoke` ctest renders 60 frames of a bot match under xvfb), ASan+UBSan and wasm
(`.github/workflows/ci.yml`). clang-format and clang-tidy never run over `third_party/`.

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
content hash) and rewrites the `.checksums`. A new corpus replay needs a manifest line.

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

**Presentation and platform (§7, v0.15).** `presentation` is float code over sim state: sim
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
human lead in mixed matches, none bot versus bot. SDKs, example bots and the contest live in a
separate repository; this repo ships no training tooling (no ONNX, no nanobind, no Python env).
`kfm_arena` is the headless match runner (modules in, replay and result line out). As built
(v0.30): the runtime is WAMR 2.4.5's classic interpreter (`kfm::wamr`, FetchContent only,
patched at configure time to read the remaining budget back; `KFM_WASM_BOTS=OFF` builds without
it, and Emscripten never builds it), `kfm_ai_wasm` is a separate target so `kfm_ai` never sees
WAMR, the budgets are 100 k instructions per tick and 10 M for instantiation and `kfm_init`,
modules are at most 4 MiB with 16 MiB of memory, and `docs/bot-abi.md` is the normative ABI and
Observation v1 layout. `engine/ai` is fixed-point only (the no-float check covers it): a bot
match reproduces bit for bit on every host. Every host names bots through `BotSpec` and seeds
slot s with `seed ^ (0x9E3779B97F4A7C15 · (s + 1))`; `engine/net`'s only bot hook is
`ServerConfig::botInputs`, and `net` and `ai` never link each other.

## Working conventions specific to this repo

- Strong typedefs for `Tick`, `PlayerId`, `EntityId`; never pass raw integers for these.
- Content (abilities, power-ups, arenas, modes) is data under `content/`, loaded into an immutable
  `ContentDB` and hashed; peers must match `content_hash` and `build_id`.
- Netcode behaviour is tested through the loopback emulator (latency/jitter/loss/reorder), never
  by hand against a live server first.
- Glossary for terms used in code and docs (tick, lead, rollback, authoritative frame, confirmed
  tick, misprediction, sub-tick timestamp, lane): `docs/architecture.md` Appendix B.

## Lessons from CI (keep a local pass a CI pass)

- Fast iteration: `cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKFM_HEADLESS=ON
  -DKFM_WEBRTC=OFF -DKFM_USE_VCPKG=OFF` builds everything but the client and the WebRTC
  transport in minutes (no SDL3/ImGui/sokol/libdatachannel); `ctest --test-dir build/headless
  -LE 'soak|network'`. Use the full `dev` preset before pushing anything that touches
  `apps/client`, and the `server-headless` preset for the soak and real-socket gates.
- CI's lint job pins clang-format 23.1.3 and clang-tidy 23.1.4 (PyPI wheels: `pip install
  clang-format==23.1.3 clang-tidy==23.1.4`); run those exact versions locally, over the whole
  tree the way `.github/workflows/ci.yml` does, and count both `error` and `warning` lines.
  The versions are recorded in architecture v0.30 (§9); bump both together.
- MSVC `/W4 /WX` differs from GCC: a `return` after Catch2's `FAIL(...)` is unreachable code
  (C4702), so write `if (!opt) FAIL(...); return opt.value_or(...)` rather than `{ FAIL(); return
  {}; }`; `size_t` passed to a `uint32_t` parameter warns (C4267) even when the value is a
  constant GCC can prove fits. clang-tidy's `bugprone-unchecked-optional-access` does not treat
  `REQUIRE(opt)` as a guard; use `if (!opt) { FAIL(); return; }` in a `void` test or `value_or`.
- Third-party sources fetched into `build/<preset>/_deps` may ship their own `.clang-tidy`;
  clang-tidy reads the nearest config for every file a diagnostic lands in, headers included,
  so `third_party/wamr/CMakeLists.txt` overwrites WAMR's with one that disables every check.
- The room (v0.42): room state never enters `WorldState` or `NetServer` (which only reports
  `RoomSettings`/`Start` events and sends the `RoomState` the host sets); policy is
  `apps/server/room.hpp` driven by `ServerHost`. Waiting and Over mean the host does not call
  `NetServer::tick()` at all (the sim leaves `MatchPhase::Lobby` on its first step); clients
  hold (`NetClient::holding()`) while their `RoomState` is not Playing. Tests and `kfm_netsim`
  that drive `NetServer` directly, and `HostConfig{}` (room off), keep the M3 flow.
- Lobby protocol changes go to `docs/lobby-protocol.md` first; the Rust suite, `kfm_lobby_tests`
  (label `network`, a fake lobby in-process) and the client's session test all pin it. Every
  envelope is a flat JSON object of strings (nested signaling rides as a string in `msg`) so
  the engine's hand-written decoder reads it. `ServerHost` must stay lobby-agnostic: the wiring
  is `LobbyAdapter` in apps/server. Additions stay backward compatible (v1.1, v0.41: a new key
  beside the old ones; a reader treats a missing key as the older lobby's meaning, as a missing
  `free` means "every listed region free"); `/healthz`'s shape is unchanged. GCC 15 at -O3 flags copy-assigning a `vector<shared_ptr>`
  as a null dereference (`-Werror` on the Release legs): move instead.
- Next milestone: M7, the bot ladder (architecture §11 "M7 sequencing"; steps 1 and 2 landed in v0.60 and v0.61). M6b
  "Play with friends" (architecture v0.40, §11 "M6b design") landed in v0.58: step 1,
  lobby protocol v1.1 (`regions` lists every region plus `free`), landed in v0.41; step 2, the
  online room (Waiting/Playing/Over beside `ServerHost`, `RoomSettings`/`RoomState`/`Start`,
  nicknames, protocol 6), landed in v0.42; step 3, the client's front end (main menu, Settings,
  Play online, the waiting screen with Copy link / Share, scoreboards, match-over), landed in
  v0.43; step 4, bots yielding (`ServerConfig::queueForBotSlots`, the room's yield rule),
  Reconnect from the UI (lobby protocol v1.2's rejoin) and the high-RTT lead check (the
  jitter-aware band, a 500 ms soak gate), landed in v0.44; the pause menu on Escape (local
  pause, online idle input and Leave room) landed in v0.46; the friends playtest (step 5) was played and
  M6b landed in v0.58. Next is M7, not started. The bot
  ladder is M7 (unchanged in content); its first steps in this repo are the `contest` mode, the
  append-only `kfm_arena` JSON result and a tagged release of the headless `kfm_arena` (step 1,
  landed in v0.60); step 2, the bots repository `minip8/kung-fu-monkeys-bots` (the C header copied
  byte for byte and diffed by its CI, the C++ layer, examples, the recipe and image, the engine
  pin in its `engine.lock`), landed in v0.61; step 3, the service, is designed in v0.63 (§11 "M7
  step 3 design": Rust/axum/sqlx, Postgres, S3-compatible blobs, a jobs table as the queue, GitHub or
  Google OAuth, gVisor on a build VM, Swiss rounds under Glicko-2) and landed there in v0.64 (`service/`,
  `deploy/`; kfm_arena baked into the service image); step 4, seasons, the Swiss scheduler, the match
  job and Glicko-2, landed there in v0.65; step 5, the `kfm` CLI (`kfm-cli` on PyPI), in v0.66, with
  replays kept 24 hours and the host `bots.kfm.minip8.com`; deployed in v0.67 (two VMs beside the game
  server, R2, `kfm-cli` 0.1.1 on PyPI, season `2026-11`); next is step 6, the replay viewer here. v0.59: a
  ladder season is a time window, not a pinned release; every match records its release tag,
  replays are served with their release's content and viewed in that release's client (the page
  under a versioned path per tag), ratings persist across a release. M5b landed in v0.35 (five PRs, one
  architecture version each, v0.31 to v0.35); its human playtest (`hill`, a 15-tick grid round)
  is pending. Spawn groups fixed the side bias between teams only: the free-for-all
  spawn-order slot bias of §12 is still open (tournaments rotate).
- The client UI (v0.43): Dear ImGui with the game font and the palette of `apps/client/ui_style.hpp`
  (no colour literals in the HUD or screens); screen state lives in headless models in
  `apps/client/core` (`menus.hpp`, `online_menu.hpp`, `scoreboard.hpp`), tested without a window;
  hud.cpp / screens.cpp only draw them. `assets/fonts/` holds source assets with their licence and
  is compiled in (never read at run time: the browser has no filesystem). Clipboard and sharing
  go through `IPlatformServices` (`ShareHooks` injected by main.cpp), never SDL or JS directly.
  v0.55: what is highlighted follows the last menu device (`core/menu_highlight.hpp`): mouse mode
  highlights only the hovered item, keys/pad mode the focus, and the first key press from mouse
  mode only reveals it. Draw menu buttons and rows through `ui::bigButton` / `ui::showsFocus` /
  `ui::pointerTakesFocus`, never from `focus()` alone. A frame in which a new ImGui window
  measures itself is not presented (`debugUiMeasuring()`); a screen's panel card drawn outside
  its window must not be relied on in that frame. The between-rounds scoreboard shows in
  RoundEnded and at match end only, never over the next countdown; a round's pause is 180 ticks
  (`round_end_ticks`).
- Fog lives in `engine/ai`, never in the sim: a `vision` change is content hash only, and the
  `all` vision must stay the pre-fog observation byte for byte (a test pins it). Hosts call
  `AgentHost::observe(sink, world, content)` after every `step`, or their bots hear no calls.
- Teams never enter `WorldState`: a free-for-all config (slot i = team i) must keep every
  checksum. New rows that hurt or push get an Enemy row and an Ally twin needing
  `Tag::FriendlyFire` on the receiving monkey (a test pins the twins); hosts fill
  `MatchConfig::teams` with `sim::teamsFor(mode, present)` (or through `resolveMatchNames`).
- Objective kinds keep their state in `MatchRules::modeData` (layouts in `src/modes/hill.cpp`,
  `capture.cpp` and `docs/bot-abi.md` §3), never in a new world-state field; a capture flag is a
  `PickupKind::Flag` pickup whose `spawner` byte is its team. The scripted generator's objective
  steering must stay Rng-free and keyed on the mode kind, or every other corpus entry moves.
- Spaces (v0.34): how monkeys move is `spaceBehaviorFor(mode.space)` (`src/spaces/`), nothing else;
  the continuous row must stay the sim as built bit for bit. Cells come only from
  `kfm/sim/spaces/grid_cells.hpp` (column 0 west, row 0 north, a cell centre = a tilemap spawn
  point). The grid latch reads the frame and the world state only (rollback-exact); contested
  cells are not rule rows. Grid content counts in turns: a grid throw's recovery equals
  `turn_ticks` and roots, a grid dash's active ticks are the slide; a mode's numbers that must
  scale with the turn need their own defs (the `_1t` twins). The contact radius is
  `Stats::radius`, never `PlayerDef::radius`, in sim code.
- Pits (v0.57): a pit starts a fall (`Tag::Falling`, `Player::fallTicks` from `[player]
  fall_ticks`, `PlayerFell`), never an elimination on the spot, and only once the whole contact
  disc is over the union of the pit boxes (`discOverPits` / `discWithinBoxes` in
  `content/arena.hpp`, exact and integer-only; the bot uses the same function). Row 60 forbids
  `Dashing`, so a dash's reach is how wide a pit it clears. `kGone` in `rules/table.cpp` holds
  `Falling`: a new row that forbids `kGone` is safe against a falling monkey. A dead (or newly
  falling) monkey's bananas in flight are released at once (`releaseBananasOf`: dropped as
  pickups, or gone in a mode whose `death_bananas` is vanish), and otherwise bananas never
  disappear (`rules/bananas.hpp`: held ones at a non-pit death in a drop mode, a catch past
  `ammoMax`, a full pickup pool all drop or stack; a new path that would destroy a banana must
  drop it instead; the exceptions are a vanish mode's death and an ammo power-up's extra
  bananas when it runs out); a
  grounded banana wholly over a pit sinks instead (`BoomerangPhase::Sinking`, never a pickup); a
  new Boomerang row must name an Outbound/Returning phase mask so it never touches a sinking one
  (a test pins it). A new
  round-ending rule must wait for falls (`anyFalling`), or a doomed last monkey wins; the round
  kinds also end a dry round (no banana left anywhere, `roundRanDry`, `[[mode]]
  dry_round_ticks`), and anything new that makes bananas must count in `bananaLeft`.
- A new mode hook or test-only mode: `MatchContent::modeBehavior` swaps the kind's
  `ModeBehavior` row in a scenario (`tests/scenario/mode_hooks_scenario.cpp`); hosts never set it.

## Git workflow

**When to commit.** Commit as you go, without being asked: at the end of each logical unit of
work, as soon as it is complete and (once a build exists) configures, builds and passes tests. A
logical unit is a system, a rule, a transport, a content def with its test, a doc decision. Do not
mix a refactor with a behaviour change. Do not leave finished work uncommitted at the end of a
turn; if something is deliberately left uncommitted (broken, half-done, exploratory), say so.
`main` stays green. Work on a branch (`feat/<topic>`, `fix/<topic>`, `docs/<topic>`), never
directly on `main`; create the branch before the first commit of a task. A design decision and
the `docs/architecture.md` update that records it go in the same commit.

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
