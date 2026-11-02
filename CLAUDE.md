# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

Kung Fu Monkeys is an online 2–10 player arena brawler (Boomerang Fu-inspired: monkeys throw
bananas that fly like boomerangs; dash is the base ability; a perfectly timed dash catches a
banana). C++23 engine, deterministic simulation, rollback netcode, browser as the primary shipping
client, with scripted bots and ML-trained agents planned.

Milestone M0 (skeleton) has landed: build system, `engine/core`, a minimal `engine/sim`, the
replay runner and the determinism gate. M1 (headless sim) is next. `docs/architecture.md` is the
single source of truth for every decision below and is versioned (status line + changelog at the
top). When a design decision changes, update the doc in the same change, bump its version and add
a changelog line. Do not let code and doc disagree silently.

## Commands

Toolchain: CMake ≥ 3.25, Ninja, a C++23 compiler: GCC ≥ 14 or Clang ≥ 19 on Linux (Ubuntu
24.04's GCC 13 and Clang 18 do not work), MSVC 2022, Apple Clang 16+, Emscripten 4+; verified on
GCC 15, Clang 21, MSVC 19.51, Apple Clang 21, Emscripten 6. Optional: vcpkg (`VCPKG_ROOT`; without it
Catch2 is fetched at configure time at the same pinned version) and emsdk (`EMSDK`, set by
`source ~/emsdk/emsdk_env.sh`) for the `wasm` preset. Build directories are `build/<preset>`.

```sh
cmake --preset dev                      # configure; presets: dev, release, asan, server-headless, wasm
cmake --build --preset dev              # build
ctest --preset dev                      # unit tests + determinism corpus + C++23 feature probe
cmake --workflow --preset dev           # configure + build + test in one go
ctest --preset dev -R kfm_core_tests    # one test executable (kfm_core_tests, kfm_sim_tests, determinism_*)
build/dev/tests/unit/kfm_core_tests "[fixed]"      # one Catch2 tag or test name
cmake --build --preset dev --target regen-replay-corpus   # after an intentional gameplay change only
tools/ci/check_sim_no_float.sh          # no float/double in engine/sim
clang-format -i $(find engine apps tests tools -name '*.cpp' -o -name '*.hpp' | grep -v sin_table)
clang-tidy -p build/dev $(find engine apps tests -name '*.cpp')
build/dev/apps/replay/kfm_replay record-empty|info|checksums|verify ...   # replay tool
python3 tools/codegen/gen_sin_table.py > engine/core/include/kfm/core/sin_table.inc  # trig LUT
```

The `wasm` preset runs the same tests under node. CI runs the lint job, the four native
toolchains in Debug and Release (Linux on Ubuntu 26.04), ASan+UBSan and wasm
(`.github/workflows/ci.yml`).

## Architecture in one screen

Read `docs/architecture.md` §2 (decisions table) first; the rest of this section says which rules
are load-bearing when writing code.

**Layering (§4).** `engine/core` → `engine/sim` → {`engine/net`, `engine/ai`,
`engine/presentation`} → `apps/`. `sim` depends only on `core` and must never include anything
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

A replay corpus with per-tick checksums is the CI merge gate across Linux/Windows/macOS/wasm and
Debug/Release. Any sim change that alters checksums is either a bug or an intentional gameplay
change that regenerates the corpus; say which.

**Gameplay is added through tables, not special cases (§5.5–5.8).**
- Abilities: `AbilityDef` (content) + POD `AbilityState` per slot + an `AbilityBehavior` function
  table indexed by `AbilityKind`. New ability = new behaviour file, content def, one registry
  line, one scenario test.
- Interactions: `CollisionSystem` emits contacts; `InteractionSystem` resolves them through the
  prioritised rules table keyed on `(kind, tags) × (kind, tags)`. New twist = new rule (+ tag).
  The dash-catch is a rule row, not code inside the dash.
- Power-ups: `PowerUpDef` grants `StatModifier`/`AbilityOverride`/`Tag`; `ModifierSystem`
  recomputes derived stats each tick.
- `SimEvent`s are the only channel from sim to presentation, scoring and RL rewards.

**Input (§5.4, §5.11).** `PlayerInput` is 5 bytes: `moveDir`, `moveMag`, `aimDir`, `buttons`,
`subTick`. Button edges are detected in `InputSystem`, which is what makes "repeat last input"
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
Never push without being asked. `main` stays green. Work on a branch (`feat/<topic>`,
`fix/<topic>`, `docs/<topic>`), never directly on `main`; create the branch before the first
commit of a task. A design decision and the `docs/architecture.md` update that records it go in
the same commit.

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
