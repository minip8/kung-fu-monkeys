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
`docs/bot-abi.md` (the bot ABI and Observation v2), `docs/lobby-protocol.md` and
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

```sh
cmake --preset dev                      # configure; presets: dev, release, asan, server-headless, wasm
cmake --build --preset dev              # build
ctest --preset dev                      # unit tests + determinism corpus + C++23 feature probe
cmake --workflow --preset dev           # configure + build + test in one go
ctest --preset dev -R kfm_core_tests    # one test executable (kfm_core_tests, kfm_sim_tests, kfm_scenario_tests, determinism_*)
build/dev/tests/unit/kfm_core_tests "[fixed]"      # one Catch2 tag or test name
tools/ci/check_sim_no_float.sh          # no float/double in engine/sim (and engine/ai)
clang-format -i $(find engine apps tests tools -name '*.cpp' -o -name '*.hpp' | grep -v sin_table)
clang-tidy -p build/dev $(find engine apps tests -name '*.cpp')
cmake --build --preset dev --target regen-replay-corpus   # after an intentional gameplay/content change only
```

The `wasm` preset runs the same tests under node and builds `kfm_client.html`. CI runs the lint
job, the four native toolchains in Debug and Release (Linux on Ubuntu 26.04, where a
`client_smoke` ctest renders 60 frames of a bot match under xvfb), ASan+UBSan and wasm
(`.github/workflows/ci.yml`). clang-format and clang-tidy never run over `third_party/`.

Everything else is in `docs/commands.md`: the other test selections and build options, and
`kfm_client`, `kfm_server`, `kfm_replay`, `kfm_arena`, `kfm_netsim`, the probes, the lobby
service and the browser build with their flags and URL parameters, the content names (modes,
arenas, loadouts) and the release flow. Read the section for a binary before running it rather
than guessing its flags.

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
`docs/bot-abi.md` is the normative ABI and Observation v2 layout (version 1 modules are still
served). `engine/ai` is fixed-point only (the no-float check covers it): a bot match reproduces
bit for bit on every host. Every host names bots through `BotSpec` and seeds slot s with
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

The rules that matter in one part of the tree live in `.claude/rules/` and load when a file
they cover is read or edited:

- `sim.md`: `engine/sim`, `engine/content`, `content/`, `apps/replay` and their tests (teams,
  objectives in `modeData`, spaces and the grid, pits, bananas never disappearing, round
  endings, serialised structs).
- `bots.md`: `engine/ai`, `apps/arena`, `tools/bots` and the hosts that step agents (fog,
  `AgentHost`, the WAMR runtime and budgets, the SDK header, the `kfm_arena --json` contract).
- `server-lobby.md`: `apps/server`, `engine/net`, `services/lobby`, `deploy/` (the room, the
  lobby protocol, a match outliving the lobby).
- `client.md`: `apps/client` (UI models, `IPlatformServices`, menu highlight, the scoreboard).

They are not in context until then, and after a compaction not until such a file is touched
again. Read the file for an area before planning or reviewing a change there without having
opened its code. A rule that must hold everywhere belongs in this file, not there.

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
must be rewritten before the PR exists, say so in the turn.

**Merging.** Merge the pull request yourself once CI is green on every leg (the native matrix,
ASan+UBSan and wasm), without waiting to be asked: `gh pr merge --merge` (a merge commit, as
the history has; never a squash or a rebase). Never merge with a check failing, pending or
skipped by a workaround, never bypass branch protection, and leave a PR alone when the user
asked to review it first or it is not one you opened. The same holds in the bots repository.
Merging releases nothing: a release is a `v*` tag (`deploy.yml` here, `release.yml` in the bots
repository), and a tag is pushed only when the user asks for that release.

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
