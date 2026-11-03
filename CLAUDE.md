# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

Kung Fu Monkeys is an online 2–10 player arena brawler (Boomerang Fu-inspired: monkeys throw
bananas that fly like boomerangs; dash is the base ability; a perfectly timed dash catches a
banana). C++23 engine, deterministic simulation, rollback netcode, browser as the primary shipping
client, with scripted bots and ML-trained agents planned.

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
and the client's `--connect` mode. Next is M5 (bots and ML). `docs/architecture.md` is the
single source of truth for every decision below and is versioned (status line + changelog at the
top). When a design decision changes, update the doc in the same change, bump its version and add
a changelog line. Do not let code and doc disagree silently.

## Commands

Toolchain: CMake ≥ 3.25, Ninja, a C++23 compiler: GCC ≥ 14 or Clang ≥ 19 on Linux (Ubuntu
24.04's GCC 13 and Clang 18 do not work), MSVC 2022, Apple Clang 16+, Emscripten 4+; verified on
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
#   record-*/bench/stats take --arena NAME --mode NAME; every command takes --content DIR (default: the
#   content compiled into the binary); `info` prints the replay's arena/mode and both content hashes
build/release/apps/replay/kfm_replay bench --ticks 36000 --players 10   # ticks/s of a scripted match
python3 tools/codegen/gen_sin_table.py > engine/core/include/kfm/core/sin_table.inc  # trig LUT
build/dev/apps/client/kfm_client [--autostart] [--bots N] [--seed S] [--overlay] [--mute] [--volume 0..100]   # playable client
build/dev/apps/client/kfm_client --content content --arena dojo --mode rounds   # edit content/*.toml while it
#   runs: local play hot-reloads the directory (~1 s) and restarts the match (--no-hot-reload to stop it).
#   Without --content: content/ next to the executable (copied by the build), else the built-in copy.
#   modes (content/modes.toml): rounds (default), deathmatch (respawns, score, clock), survival (rounds on a clock)
#   lobby: Enter/Start begins; press throw/dash on a free device to join; -/= bots. F1 overlay, P pause, O step, Esc lobby
#   lobby settings panel: arrows/WASD or a joined pad's d-pad/stick (Up/Down choose, Left/Right change) or the mouse:
#   arena (with preview), mode, power-up toggles, bots, volume, mute; saved between runs (SDL pref dir / browser
#   localStorage), flags win: --arena NAME --mode NAME --powerups all|none|a,b ; --no-settings ignores the saved file
#   browser: kfm_client.html?arena=pillars&mode=deathmatch&powerups=shield,big_banana
#   audio: procedural sounds, silent without a device (F1 > Audio: mute, volume); browser: ?mute&volume=50
ctest --preset dev -R kfm_client_tests                 # headless client core (clock, session, lobby, options)
ctest --preset dev -R kfm_content_tests                # TOML loader, decimal conversion, base kit, arena 0, mode 0 == defaultContent()
build/dev/apps/client/kfm_client --exit-after-frames 60 --screenshot out.ppm   # headless render check
build/dev/apps/render-probe/kfm_render_probe --players 6 --exit-on hit --exit-after-frames 6000 --screenshot hit.ppm
#   renders a scripted match; --exit-on: dash-ring|perfect-catch|auto-catch|hit|throw|elimination|catch-window
#   --arena pillars|corridors|island draws an arena with walls / pits / mud (content/arenas/*.toml)
build/dev/apps/client/kfm_client --capsules                # placeholder capsules instead of the skinned test rig
#   (the rig is generated by the build from tools/assets/gen_test_rig.py; never commit exported glTF)
build/dev/apps/replay/kfm_replay stats --players 4 --matches 20   # feel-tuning stats over scripted matches
build/dev/apps/client/kfm_client --arena dojo_powerups --bots 3   # power-ups: the dojo plus four spawners (content/powerups.toml)
source ~/emsdk/emsdk_env.sh && cmake --workflow --preset wasm && python3 tools/web/serve.py [--page kfm_render_probe.html]
#   then open http://127.0.0.1:8000/kfm_client.html?autostart&bots=3 (browser build of the same client)
python3 tools/web/check_browser.py 'http://127.0.0.1:8000/kfm_client.html?autostart&bots=3&overlay' --out shots  # Playwright check
build/dev/apps/server/kfm_server --port 9000 --bots 3 [--token T] [--max-players N] [--exit-after-seconds N] [--stats-every S]   # dedicated server
#   also --content DIR --arena NAME --mode NAME (kfm_netsim and kfm_render_probe take the same three)
#   and --powerups all|none|name,name (the power-up set of every match; kfm_replay record-*/bench/stats too)
build/dev/apps/netsim/kfm_netsim --clients 10 --rtt 100 --jitter 10 --loss 2 --seconds 30 [--csv run.csv]   # soak over the emulator; exits non-zero on any desync
ctest --preset dev -L soak                     # the two netsim gates (10 clients at 100 ms / 2% and 300 ms / 10%)
cmake --workflow --preset server-headless      # headless Release build of kfm_server + tests (no SDL)
build/dev/apps/client/kfm_client --connect ws://127.0.0.1:9000 [--token T] [--spectate] [--players N] [--stun URL]   # join a server
#   browser: kfm_client.html?connect=ws://host:9000&token=T ; Esc leaves; pause/step/time scale/tuning are off online
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
- `SimEvent`s are the only channel from sim to presentation, scoring and RL rewards.

**Input (§5.4, §5.11).** `PlayerInput` is 5 bytes: `moveDir`, `moveMag`, `aimDir`, `buttons`,
`subTick`. The stick is idle iff `moveMag == 0`; `moveDir` is always an angle. Button edges are detected in `InputSystem`, which is what makes "repeat last input"
a safe remote prediction. `subTick` is the fraction of the tick at which the last button edge
happened; it is used only by time-aware rules (perfect-dash window judged against swept
time-of-impact, tie-breaking), never to move entities within a tick. The same struct is the
bot/RL action space.

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

**Bots and ML (§8).** Agents implement `IAgent` and produce `PlayerInput` exactly like a human;
they live outside the sim. Server-side bots write into the authoritative frame directly. The
Python module (`bindings/python`, nanobind) exposes a vectorised headless env over the same sim.

## Working conventions specific to this repo

- Strong typedefs for `Tick`, `PlayerId`, `EntityId`; never pass raw integers for these.
- Content (abilities, power-ups, arenas, modes) is data under `content/`, loaded into an immutable
  `ContentDB` and hashed; peers must match `content_hash` and `build_id`.
- Netcode behaviour is tested through the loopback emulator (latency/jitter/loss/reorder), never
  by hand against a live server first.
- Glossary for terms used in code and docs (tick, lead, rollback, authoritative frame, confirmed
  tick, misprediction, sub-tick timestamp, lane): `docs/architecture.md` Appendix B.

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
Scopes: `core`, `sim`, `net`, `ai`, `presentation`, `platform`, `content`, `apps`, `bindings`,
`docs`, `ci`. Omit the scope only for repo-wide changes.

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
