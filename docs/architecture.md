# Kung Fu Monkeys — Architecture Design

Status: v0.79 (2026-11-11), a living document: it describes the system as built and deployed,
and in §11 and §12 what is planned and what is open. Working title taken from the repo name.

Changelog: `docs/architecture-changelog.md`, one entry per version, oldest first. A design
change updates this document, bumps the version above and appends an entry there, in the same
commit. "The v0.N changelog line" in the text below means that file's v0.N entry.

A top-down online arena brawler inspired by Boomerang Fu. Monkeys throw bananas
that fly like boomerangs and return. Movement, throw and dash are the base verbs,
power-ups are picked up in the arena, and the signature twist is that a perfectly
timed dash onto a flying banana catches it. 2–10 players online, C++ engine,
latency-first netcode, a simulation that bots can drive headlessly, and a public bot API so
anyone can write one.

---

## 1. Goals and constraints

| Requirement | Architectural consequence |
|---|---|
| Fast, precise party brawler (one-hit kills by default, timing windows) | Fixed 60 Hz tick, exact timing semantics, zero local input delay |
| 2–10 players online, smooth | Server-authoritative, input-synchronised rollback netcode, tiny packets |
| Modular gameplay: dash now, more abilities and twists later | Data-driven definitions, table-driven behaviours, an interaction rules table |
| Engine in C++ | C++23, CMake, small set of battle-tested libraries, strict layering |
| Bots written by anyone, trained or not | Deterministic, headless, allocation-free sim; agents are just input providers; a sandboxed, instruction-metered wasm bot ABI (§8) |
| Reproducibility / debugging | Determinism + input-only replays + per-tick checksums |
| Ship in the browser first | WebRTC transport, WebGL2 renderer, single-threaded client, dedicated servers; native build kept for development |

Non-goals for v1: consoles, more than 10 players, 3D gameplay or physics-heavy mechanics,
user-generated content, mobile touch controls. The browser is the primary shipping client (§13);
the native client is kept as the development build, and the Steam path stays open behind the
platform seams.

---

## 2. Decisions at a glance

| Area | Decision | Why |
|---|---|---|
| Simulation | Deterministic, fixed timestep (60 Hz) with sub-tick input timestamps, fixed-point math, plain-old-data (POD) world state with fixed-capacity pools | Rollback by `memcpy`, replays are just inputs, Linux servers, native clients and the browser build agree bit-for-bit, bot matches are reproducible |
| Layering | `core` → `sim` → {`net`, `ai`, `presentation`} → apps. `sim` has no I/O, no platform code, no allocation after init | Same sim binary logic in client, server, replay tool, tests and the match runner |
| Netcode | Server-authoritative deterministic sim with **input synchronisation**: clients send inputs, server relays authoritative inputs, clients predict everyone and roll back | GGPO-style feel (no local input delay) plus a trusted authority, join-in-progress and resync; bandwidth independent of entity count |
| Transport | WebRTC data channels as the single production transport (libdatachannel natively, the same C++ API via datachannel-wasm in the browser); in-process loopback with network emulation for tests; GameNetworkingSockets only if a Steam build happens. All behind one `ITransport` with an unreliable lane and a reliable lane; our message layer (input frames, redundancy, clock sync) sits above | One transport to harden, built-in encryption, browsers and native clients share a match with no bridging; the game netcode never knows which transport is in use |
| Gameplay modularity | Content defs (abilities, power-ups, arenas, modes) + behaviour registries keyed by kind + a prioritised interaction rules table | New ability or twist = new file + def + registration, no edits to existing systems |
| Modes and spaces | A mode is content along five independent axes (win condition, roster, baseline rules, objectives, space), each behind its own seam; the grid is a `Space` of the same sim in which monkeys move cell to cell while bananas stay continuous; fog and communication are computed on the observation side | A new mode is TOML plus one behaviour file; the grid reuses flight, collision, the rules table, netcode, rendering and the bot ABI unchanged |
| Presentation | Real-time low-poly 3D over a strictly 2D sim: fixed tilted camera, skinned glTF characters, flat/toon shading, blob shadows mark true positions. SDL3 (window/input/audio) + sokol_gfx, Dear ImGui debug UI | Rendering is a read-only consumer of sim state; smooth facing matches continuous aim; sokol_gfx has WebGL2/WebGPU backends for the browser build |
| Bots | A bot is a wasm module behind a versioned C ABI (observation in, `PlayerInput` out), sandboxed and metered in instructions per tick; `IAgent` wraps modules and the built-in bot alike; SDKs, example bots and any training live in a separate repository, no training stack in the engine | Same action space for humans and every bot; any language; a match is reproducible from seed plus modules; fairness by fuel, not wall clock |
| Tooling | CMake presets, vcpkg, Catch2, Tracy, sanitizers, clang-format/clang-tidy, CI with a cross-OS determinism gate | Determinism is enforced, not hoped for |
| Platforms | Web first: the browser is the shipping client, native is the dev/debug/profiling build, Steam is an optional later add. Platform code is confined to `ITransport` and `IPlatformServices` | Join friction decides a party game; everything below the seams is shared, so Steam stays a cheap option |
| Hosting (v0.37) | A Rust lobby service (rooms, tokens, signaling relay, server registry) as the only public TCP endpoint; dedicated `kfm_server` processes on one small VM per region that dial the lobby; the client on a static CDN. Container images plus systemd units, no provider-specific service; Google Cloud now, Hetzner later | A link puts friends in a match; game VMs need only UDP open; the whole deployment moves between providers unchanged |

---

## 3. System overview

```
                      ┌────────────────────────────────────────────────────┐
                      │                    Client app                      │
  keyboard / pad ────▶│ InputSampler ─▶ PlayerInput ──┐                    │
                      │                               ▼                    │
                      │  NetClient ◀──▶ Predictor / Rollback ──▶ sim::step │
                      │     ▲         (ring buffer of saved WorldStates)   │
                      │     │                     │ state + SimEvents      │
                      │     │                     ▼                        │
                      │     │              Presentation                    │
                      │     │   (interpolation, smoothing, VFX, audio, UI) │
                      └─────┼────────────────────────────────────────────┘
                            │ WebRTC (browser and native; GNS only for Steam)   ↑ my inputs   ↓ authoritative inputs, snapshots
                      ┌─────┼────────────────────────────────────────────┐
                      │  Dedicated server (headless, N matches/process)  │
                      │  NetServer ─▶ InputCollector ─▶ sim::step ─▶ bcast│
                      │  server-side bots (IAgent) ─┘                    │
                      └──────────────────────────────────────────────────┘

  The same `sim` library runs in: client (prediction), server (authority),
  replay viewer, test harness, headless match runner.
```

---

## 4. Repository and module layout

```
kung-fu-monkeys/
  CMakeLists.txt  CMakePresets.json  vcpkg.json  .clang-format  .clang-tidy
  cmake/                 toolchain helpers, warnings, sanitizer presets
  docs/                  this document, netcode notes, content authoring guide
  engine/
    core/                Fixed (Q16.16), Vec2, Angle, Pool<T,N>, RingBuffer, BitWriter/Reader,
                         Rng (xoshiro128**), hash (xxHash), log, assert, strong typedefs
    sim/                 WorldState, step(), systems/, abilities/, rules/, content defs, events,
                         serialization (visitor), checksum
    content/             TOML content files -> validated ContentDB (toml++), the embedded copy,
                         app-side loading (--content/--arena/--mode) and hot-reload polling
    net/                 messages/ (bit-packed frames, redundancy), transport/ (webrtc, loopback+emulator, optional gns),
                         clock sync, NetClient, NetServer, snapshot codec
    ai/                  IAgent, Observation builder, the reference bot, WasmAgent (bot modules
                         over the wasm runtime, instruction-metered)
    presentation/        renderer (meshes, skinning, materials), animation, camera, audio, vfx, ui,
                         interpolation+smoothing
    platform/            SDL3 window/input/audio/gamepads, filesystem, main loop, config,
                         services/ (IPlatformServices: steam, web, null)
  content/               data files (*.toml: player, abilities, boomerang, arenas, modes; later
                         power-ups) and source models/ audio/
  apps/
    client/              playable client (also hosts local multiplayer)
    server/              headless dedicated server
    replay/              replay recorder / verifier / analyser (`kfm_replay`; the viewer is
                         the client's `--replay`, v0.68)
    netsim/              N clients + 1 server in one process over an emulated network
    arena/               headless match runner: bot modules in, replay and result line out
  services/
    lobby/               the Rust lobby service: room codes, connection tokens, signaling relay,
                         server registry (§13.5, v0.37)
  deploy/                container images (server, lobby), systemd units, Caddyfile, VM scripts
  tests/                 unit/ scenario/ determinism/ netcode/ fuzz/ bench/
  tools/                 content validator, replay corpus, CI scripts
```

Dependency rules (enforced via CMake target link visibility):

- `core` depends on nothing but the standard library.
- `sim` depends only on `core`. It never includes `net`, `presentation` or `platform`.
- `net`, `ai`, `presentation` depend on `sim` and `core`, never on each other.
- `content` (v0.18) depends on `sim` and `core` (and privately on toml++); only apps and tests
  link it, so `net`, `ai` and `presentation` keep taking a `ContentDB` and never parse files.
- Apps wire everything together. Only apps, `content` and `platform` touch the OS (`content`
  reads files and nothing else).

---

## 5. Simulation (`engine/sim`)

### 5.1 Determinism contract

- Fixed timestep: `kTickHz = 60` (one constant; 120 is a possible later experiment).
- Fixed-point arithmetic: `Fixed` = Q16.16 stored in `int32_t`, `int64_t` intermediates.
  Range ±32767, resolution 1/65536. Trig via lookup tables, `isqrt` integer square root,
  vector normalisation with explicit guards. Presentation converts to `float` at the boundary.
  The sine table is generated by a checked-in script and committed as source, never computed
  at build time: compile-time floating point is not guaranteed to round identically across
  compilers, and the table is part of the determinism contract.
- Forbidden inside `sim`: floating point, wall-clock or OS randomness, pointers in state,
  heap allocation after init, iteration over hash containers, exceptions, RTTI, threads.
- `Rng` lives inside `WorldState` and is seeded per match.
- Content definitions are immutable per match and hashed (`content_hash`); peers must also
  match `build_id`. Both are exchanged at connect.
  The build id as built (v0.70; before it every build sent 0, so a change to sim code alone
  passed the handshake and a stale cached page looped on checksum resyncs against a newer
  server): one CMake cache variable, `KFM_BUILD_ID` (1–8 hex digits, `cmake/KfmBuildId.cmake`),
  compiled into the generated `<kfm/build_id.hpp>` (`kfm::kBuildId`, target `kfm::build_id`)
  that every host reads: the client's `Hello` and lobby request, the server's `Hello` check and
  lobby `register`, `kfm_replay`'s and `kfm_arena`'s replay header (`buildHash`) and
  `kfm_arena`'s JSON (`build_hash`). The engine keeps taking it as configuration
  (`ClientConfig`/`ServerConfig::buildId`, default 0), so its tests are the same in any build.
  The default, 0, is an unstamped development build: local builds match each other, and the
  replay corpus (headers of 0) is regenerated from unstamped builds only. It is never derived
  from the working tree, so a normal build never changes it. A release stamps it: the deploy
  workflow configures the server image, the page and the `kfm_arena` archives of a run with the
  first 8 hex digits of the commit it builds (`00000001` should those all be zero), so every
  artefact of a tag carries the same non-zero id; from the commit rather than the tag name,
  because the id must change whenever the code does and a manual run has no tag. A client
  refused for it says "version mismatch (reload the page or update the client)" (the
  `Reject{BuildMismatch}` of a direct connect; the lobby refuses `mismatch`, shown as "Version
  mismatch: reload the page or update the client"). The replay viewer shows a replay's build
  hash and never checks it (§7).
- Enforcement: a replay corpus is run in CI on Linux, Windows and macOS, Debug and Release;
  per-tick checksums must be identical. This is a merge gate, not a nice-to-have.
  `tools/ci/check_sim_no_float.sh` (lint job) rejects float types, float literals, the `Fixed`
  float conversions and `<cmath>` anywhere in `engine/sim` (v0.14).

Why fixed-point instead of floats: floats can be made deterministic on one toolchain, but the
server (Linux, GCC/Clang) and clients (MSVC/Clang, Windows/macOS) differ in compilers, libm and
FMA contraction. Fixed-point removes that entire bug class. The cost is a small math library and
some care; the performance cost in a 2D game with ~100 entities is nil.

### 5.2 World state layout

```cpp
struct WorldState {
  Tick        tick;
  Rng         rng;
  MatchRules  rules;                               // mode, phase, scores, timers
  Pool<Player,    kMaxPlayers>    players;         // 10
  Pool<Boomerang, kMaxBoomerangs> boomerangs;      // 64
  Pool<Pickup,    kMaxPickups>    pickups;         // 32
  // Arena geometry (walls, hazards, zones, spawners) is immutable content: the
  // arena the match selected (MatchContent::arena), never part of state.
};
static_assert(std::is_trivially_copyable_v<WorldState>);
```

- Trivially copyable, roughly 10–30 KB: `save == memcpy`, `load == memcpy`. Rolling back
  10 ticks costs microseconds.
- `Pool<T, N>`: fixed array + alive bitset + per-slot generation; handles are
  `EntityId{uint16_t index, uint16_t generation}`. Slots are never compacted, so indices are stable.
- Entities are plain structs composed of component structs (`Transform`, `Body`, `AbilitySlots`,
  `ModifierList`, `Tags`, ...). We deliberately use "pools per entity kind" rather than a general
  ECS: the entity zoo is small and known, and this keeps rollback and determinism trivial. If the
  zoo grows, migrate to archetype SoA pools behind the same `Pool` interface. EnTT is the
  fallback if a general registry is ever needed, at the cost of slower snapshots.
- Serialization uses a visitor (`template <class V> void visit(V&, Player&)`) that yields
  read, write, checksum and debug dump from one function. Network snapshots, replays and
  checksums always go through the visitor, never raw memory (padding bytes are not deterministic).
  The reading visitor validates what it decodes (v0.14): an enum at or above its value count, an
  alive player whose id is not its slot, a player reference (banana owner, round winner) that is
  neither invalid nor a slot, an ability `defIndex` past the def table, or a tag bit above
  `Tag::kCount` fails the read, so a network snapshot cannot put a value into the state that a
  system would index or shift by. v0.20 adds a power-up index past `kMaxPowerUps`, a pickup
  spawner past `kMaxPickupSpawners`, a banana `pierce` above 1 (the field is gone since v0.28)
  and `grantedTags` bits above `Tag::kCount`.
- Every visited field is visited by construction (v0.70): each `visit()` overload in
  `kfm/sim/visit.hpp`, state and content alike, is followed by a
  `static_assert(fieldCount<T>() == n)`, where `kfm::fieldCount` (`kfm/core/field_count.hpp`) is
  the aggregate arity (the largest k for which `T{a0, …, ak-1}` with placeholders that convert to
  anything is well-formed, probed downwards so no probe leaves a member uninitialised; an array
  or nested struct is one field). A field added to a serialised struct and forgotten in its
  `visit()` (which would escape the checksum, snapshots and the content hash at once, and so
  every desync check) is a compile error until it is visited and the count bumped. A field
  visited only in part counts once, with a comment saying how: a `Pool`'s dead slots, a
  `ModifierList`'s tail and an `InputFrame`'s absent inputs are canonicalised, never serialised.
  The same assert pins `MatchConfig` and `ReplayHeader` to the replay header codec, and the
  checksum and content-hash mutation tests (`visit_test.cpp`, `content_test.cpp`) carry one
  mutation per field per struct, their count pinned the same way.
- Power-up fields (v0.20): a banana's `speedMul` and `radiusMul` (after `damage`; a `pierce` byte
  sat between them until v0.28), a pickup's `spawner` and a player's `grantedTags` are visited unconditionally like
  every other field; adding them changed every world checksum and the corpus was regenerated.
  A `ModifierList` visits only its live prefix, as before.
- Mode counters (v0.23): `MatchRules`' per-player `deaths`, `respawnTicks` and `invulnTicks`
  are visited unconditionally after the M1 fields, like every other field. Adding them changed
  every world checksum (the corpus was regenerated); a new state field for a real feature is
  the legitimate case for that, not a reason for a second encoding.

### 5.3 Tick pipeline

```
step(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& events
     [, StepStats& stats]):
   1. RulesSystem::preTick      round start/end, spawn waves
   2. InputSystem               decode PlayerInput → Intent; press-edge detection; space.latch (facing, grid commitments)
   3. AbilitySystem             activation, phase state machines, cooldowns (Dash, Throw, …)
   4. MovementSystem            space.move: intent × derived stats → velocity, integrate sliding along walls, arena clamp (continuous); the slide to the committed cell (grid)
   5. ProjectileSystem          boomerang flight: outbound → return steering → grounded; wall bounces
   6. CollisionSystem           broadphase (uniform-grid spatial hash) + swept narrowphase with time-of-impact, emits Contacts
   7. InteractionSystem         resolves Contacts via the rules table (hit, catch, dash attack, pickup)
   8. ModifierSystem            expire, recompute derived Stats (incl. slow hazards), overrides and tags, tick timers
   9. LifecycleSystem           eliminations, respawns, pit falls (v0.57), despawns, power-up spawners (v0.20)
  10. RulesSystem::postTick     win conditions, scoring
  11. s.tick++
```

Broadphase: a uniform grid over the arena bounds, rebuilt every tick by counting sort into
fixed-capacity arrays local to `CollisionSystem` (derived data, never part of `WorldState` or
snapshots). Moving entities are inserted by their swept AABB (start, end, radius). Candidate
pairs are deduplicated and sorted by `(kindA, indexA, kindB, indexB)` before the narrowphase,
so contact order does not depend on cell layout. Contacts are Player × Boomerang,
Player × Pickup and (v0.16) Player × Player; a player pair is listed once with
`indexA < indexB`, both players moving, radii from `Stats::radius` (v0.34; base `PlayerDef::radius`), and its contact point is
the midpoint of the two centres at `tHit` so it reads the same from either side. Player pairs are
purely geometric: every touching pair is a contact whatever its tags (at most 45 with 10
players), and the rules table decides what a touch means. A brute-force all-pairs broadphase stays in
the tests as the oracle: a property test asserts both produce the same pair set. A banana's
contact radius is `BoomerangDef::radius × Boomerang::radiusMul` (v0.20, §5.8).

Scratch capacities (v0.70). The candidate list holds every possible pair (`kMaxCandidatePairs`
1024 ≥ 10 × (64 + 32) + 45, a `static_assert`), so a push past it is asserted, never counted. The
contact list (`kMaxContacts` 256) is not the worst case: every candidate pair can be a contact and
a player in every hazard adds one per hazard, 1165 in all, which a unit test builds (every
entity on one spot). Sizing the list for it cost about 1 µs a tick (+50% in `kfm_replay bench`)
for the per-tick clears alone, so instead the overflow stays counted (`Contacts::dropped`) and
`step()` reports it through an optional `StepStats& stats` (`contactsDropped`; derived, never
state, never read by the sim). A dropped contact is a hit, catch or pickup that did not happen,
deterministically on every peer; `kfm_corpus_tests` asserts no corpus tick drops one. Since
v0.74 `kfm_replay bench`, `verify`, `stats` and `record-scripted --stats` read it too and say
so on stderr when a run dropped any (the total, the ticks affected, the worst tick), and
`verify --fail-on-dropped-contacts` makes a drop an error; another tool that wants it (the
arena runner) reads `StepStats` the same way.

Power-up spawners (v0.20) run at the end of `LifecycleSystem`, not in `RulesSystem::preTick`:
entity creation and removal already live there (grounded bananas become pickups in the same
place), a pickup spawned at the end of tick T is touchable from T+1 like a grounded banana, and
the spawner timeline is a function of `MatchRules::ticksInPhase` that the post-tick has not yet
advanced. The order of the ten systems is unchanged.
the tests as the oracle: a property test asserts both produce the same pair set.
Slam reach (v0.21): a player holding `Tag::Slamming` is inserted with its slam's reach
(`param0`) as radius, and a pair is tested at the body radii first, then at the widened ones;
a contact found only the second way carries `Contact::reach` (§5.6).
Statics (v0.22) never enter the grid: walls are resolved while integrating (steps 4 and 5,
§5.10), and after the narrowphase `CollisionSystem` appends one Player × Hazard contact per
hazard box holding a player's centre at the end of the tick (player slot order, then hazard
index), with `tHit` the centre's entry time into the box and `b` the hazard's index.

Systems are stateless free functions; the order is fixed and documented; each reads state and
inputs and writes state and events. Nothing else mutates `WorldState`.

Rules dispatch (v0.23). `RulesSystem` owns the phase machine every mode shares: Lobby →
(`MatchStarted`, round 1) RoundStarting (the present slots spawned at the arena's spawn points,
`roundStartTicks` of countdown) → Playing → RoundEnded (`roundEndTicks`: 180 ticks, 3 s, in
every shipped mode since v0.55, was 120; only the round kinds enter it, a clock kind's `endMatch`
goes straight to MatchEnded, so their value only feeds the Observation) → next round, or
MatchEnded (terminal). What ends a round or the match and what an elimination means are the
mode's: a `ModeBehavior` table indexed by `ModeDef::kind` (`kfm/sim/modes/registry.hpp`, one
file per mode under `src/modes/`, shared steps in `src/modes/mode_flow.*`), constexpr like the
ability registry. `onPlaying(s, content, sink)` runs in post-tick on every Playing tick after
`ticksInPhase` advanced; `onEliminated(s, content, victim, killer, sink)` runs in the damage step
right after `PlayerEliminated`. `ticksInPhase` is the mode clock: a time limit counts Playing
ticks, and `TimerWarning` fires with 10, 3, 2 and 1 s left. New mode = behaviour file +
`ModeKind` + its TOML name + one registry row + a scenario test.

| Kind | Round / match end | Elimination | Notes |
|---|---|---|---|
| `rounds` | at most one monkey left (two or more present): the survivor wins the round (none: draw); first to `roundsToWin` round wins takes the match | nothing | the M1 rules; never sets a mode counter |
| `deathmatch` | one round; the first to `scoreToWin` eliminations, or the leader once `timeLimitTicks` of play pass (a tied lead: draw) ends the match (`RoundEnded` + `MatchEnded`) | +1 score for the attacker (`ScoreChanged`), +1 death, respawn due in `respawnTicks` | a present slot without a monkey or a pending respawn (a late joiner) is scheduled like a respawn; a slot that left keeps its monkey until eliminated, then forfeits its score |
| `survival` | as rounds, plus a per-round `timeLimitTicks`: on timeout the survivor with the most hp wins the round, then the one with the fewest deaths this match, else a draw | +1 death | no respawn within a round |
| `team_rounds` (v0.32) | monkeys of at most one team left (two or more teams present): that team wins the round (none: draw), credited to its lowest surviving slot; the first team whose slots' round wins sum to `roundsToWin` takes the match | nothing | one team present never ends a round |
| (v0.57, the round kinds above) | a dry round: no banana in a hand, out (one sinking into a pit is gone) or lying around and no ammo power-up to come ends as the clock-out would (rounds, team rounds: a draw; survival: its timeout winner), at once without pits, after `dry_round_ticks` (180) dry ticks with them | | `roundRanDry` in `mode_flow`, `MatchRules::dryTicks`; the parry-then-fall softlock |
| `team_deathmatch` (v0.32) | one round; the first team whose slots' eliminations sum to `scoreToWin`, or the leading team once `timeLimitTicks` pass (a tied lead: draw); `MatchEnded` names the team's present top scorer | +1 score for an enemy attacker only (an ally's or one's own scores nothing), +1 death, respawn as deathmatch | a slot that leaves keeps its score: it is the team's |
| `hill` (v0.33) | one round; every Playing tick, if the monkeys in zone 0 are of exactly one team, that team's hold clock advances, and at `hillTicks` the team scores a point (credited to its lowest slot in the zone) and that clock alone restarts; a contested or empty zone holds every clock; the first team to `scoreToWin` points, or the leading team once `timeLimitTicks` pass (a tied lead: draw) | +1 death, respawn as deathmatch; nobody scores | `HillHolderChanged` when the sole holder changes; a leaver keeps its score |
| `capture` (v0.33) | one round; team t's flag starts at the centre of zone t; a carrier in its own team's zone while its own flag is home scores a capture (credited to the carrier) and the flag goes home; the first team to `scoreToWin` captures, or the leading team once `timeLimitTicks` pass (a tied lead: draw) | +1 death, respawn as deathmatch, nobody scores; a carrier drops the flag as a pickup where it stood | a dropped flag no enemy picks up for `flagReturnTicks` goes home; its own team cannot touch it |
| (v0.57, every kind: the banana economy) | the point kinds' `catch_score` (a perfect catch of an enemy's banana) and `suicide_score` (an elimination with no killer, after the kind's own hook: `reportElimination`) move the slot's signed score and count toward `scoreToWin`; shipped: deathmatch and team_deathmatch +1 / −1 | `death_bananas`: drop (the round kinds) or vanish (the respawn kinds: held and in flight go with it) | `banana_regrow_ticks` (hill, capture: 480): an empty hand grows a banana (`Player::regrowTicks`, `LifecycleSystem`); a regrowing mode never runs dry; hill and capture respawn in 300 ticks |

Respawn (`LifecycleSystem`, v0.23) is driven only by the timers: during Playing a dead present slot
with a pending `respawnTicks` counts down before this tick's despawns and respawns at 0, so a
player eliminated on tick T is back on tick T + `respawnTicks`. It takes the spawn point whose
nearest other monkey is farthest away (squared distance in raw fixed point), the world RNG
choosing among equal points (one draw per respawn), and gets `Tag::Invulnerable` with
`invulnTicks = respawnInvulnTicks`, which counts down on the same pass and clears the tag at 0:
protected for the interactions of the next `respawnInvulnTicks` ticks. The damage (30) and
dash-attack (25) rows already forbid `Invulnerable` on the victim; a protected monkey can still
attack and catch. A round start clears every timer.

Mode seams (v0.29). The table above covers one axis of a mode, the win condition. A mode varies
along five, and each has its own seam, so a new mode never needs a switch inside a system:

| Axis | Seam | Where |
|---|---|---|
| Win condition and scoring | the `ModeBehavior` table, now four hooks: `onRoundStart(s, content, sink)` after the shared spawn and before the countdown (objective placement, counters), `onPlaying`, `onEliminated`, `onPickupTaken(s, content, player, pickup, sink)` right after a pickup row applied; plus `MatchRules::modeData`, eight `uint32_t` the mode owns, visited unconditionally and cleared at match start | `src/modes/` |
| Roster: teams | `MatchConfig::teams` → `MatchContent::relation(a, b)` (Self, Ally, Enemy); a team's score is the sum of its slots' `scores` through `teamScore()`; spawn groups (§5.10) | §5.4, §5.10, §6.6 |
| Baseline rules | `ModeDef::baseline`, a power-up the mode names, granted at every spawn with no expiry: stats, overrides and tags through the existing `ModifierSystem`; a rule row that is on or off in a mode keys on a tag the baseline grants (friendly fire, §5.6) | §5.8 |
| Objectives | arena zones and `zoneOccupants()`, `PickupKind::Flag` with `Tag::Carrying`, boomerang tags | §5.10 |
| Space | `ModeDef::space` and the `SpaceBehavior` table | §5.12 |

Teams never enter `WorldState`: like the power-up set (v0.24) they ride in `MatchConfig` and
`MatchContent`, the replay header and the join messages, so free-for-all content keeps every
world checksum. A free-for-all config gives every slot its own team. Kinds added with the
seams: `team_rounds` (the last team standing wins the round), `team_deathmatch` (a team's
eliminations to `scoreToWin`), `hill` (a team scores a point for every `hillTicks` it holds the
zone alone; a contested or empty zone holds the clock, it never resets), `capture` (carry the
flag pickup into the own zone; an eliminated carrier drops it where it stood, and a dropped flag
nobody touches for `flagReturnTicks` returns to its zone). Each is a behaviour file plus a
scenario test, like the three kinds before them. Candidates that wait for a use: `hot_potato`
(a fused banana tag), `infection`.

As built (v0.31, step 1 of §11's M5b sequence): `ModeBehavior` is `{onRoundStart, onPlaying,
onEliminated, onPickupTaken}` in that order (Appendix A). `onRoundStart` is the last statement
of `RulesSystem`'s shared round start, so it sees the round's spawned monkeys, an empty banana
and pickup pool, `RoundStarted` already in the sink and the phase RoundStarting with
`ticksInPhase` 0 (a zero-length countdown still turns into Playing after it, in the same
pre-tick). `onPickupTaken(s, content, player, pickup, sink)` is called by the two pickup rows
themselves (50 banana, 51 power-up) as their last step, after the despawn and `PickupTaken`, with
a copy of the pickup as it lay; a row that declines (full ammo, a full modifier list) calls
nothing. `rounds`, `deathmatch` and `survival` use the shared no-ops `modeNoRoundStart` and
`modeNoPickupTaken`. `MatchRules::modeData` is `std::array<uint32_t, kModeDataWords = 8>`,
cleared in the Lobby → `MatchStarted` transition (not per round: a mode that wants per-round
state clears it in its `onRoundStart`) and visited after the other `MatchRules` counters. Every
hook call site goes through `modeBehaviorOf(content)`, which returns `*content.modeBehavior`
when a test set that pointer and the kind's row otherwise: the seam lets the scenario suite run
a probe mode (rounds plus counters in `modeData`) without a `ModeKind`, a loader name or a
registry row that would ship; hosts never set it, and it is not part of any config, replay or
message.

As built (v0.32, step 2: teams). The team kinds are two behaviour files
(`src/modes/team_rounds.cpp`, `team_deathmatch.cpp`) over shared helpers in `mode_flow`
(`teamsPresent`, `teamsAlive`, `lowestAliveOf`, `teamScoreLeader`, and deathmatch's roster as
`deathmatchRoster(…, forfeitScore)`, which deathmatch calls with `true`, so its code path is
unchanged). Scores stay per slot (`ScoreChanged`, `RoundEnded`/`MatchEnded` name a slot); a
team's score is always derived, `MatchContent::teamScore(scores, team)`. `endRound` judges the
match on the winner's team score, which in a free-for-all (team i = slot i) is the slot's own:
the rounds and survival code paths are bit for bit the same. Both kinds judge the teams the
config holds, not `ModeDef::teams`, so a free-for-all config makes them play like `rounds` and
`deathmatch`. The round winner of `team_rounds` is the winning team's lowest surviving slot
(one slot, so the sum stays the number of rounds won); presentation shows "Team N". Spawn
groups at the round start (§5.10): an arena with one group keeps the M1 code (one RNG offset
over every point, present slots in slot order); with several, `RulesSystem` draws one offset
per group in group order and each team's present slots take their group's points from it,
wrapping; the respawn's farthest-point choice is restricted to the slot's group
(`spawnRangeOf`).

As built (v0.33, step 3: objectives). Zones are `ArenaDef::zones` (§5.10) and
`zoneOccupants(s, arena, k)` (`kfm/sim/zones.hpp`) is their only reader: the mask of the alive,
not `Eliminated` monkeys whose centre lies in the closed box (a zone the arena lacks holds
nobody). Neither kind adds a world-state field: both keep their state in `modeData` and clear it
in `onRoundStart` (they play one round, so that is the match start). `hill`
(`src/modes/hill.cpp`): words 0–4 hold the ten teams' clocks as u16 halves (team t in word t/2,
low half for even t), word 5 the sole holder + 1 (0 = nobody); the credited slot is the team's
lowest slot standing in the zone (step 2's `team_rounds` convention applied to the zone, so a
point goes to someone who held it). `capture` (`src/modes/capture.cpp`): word t is team t's flag
(bits 0–1 Home/Carried/Dropped, 8–15 the carrier's slot, 16–31 ticks since the drop); there is
one flag per team as far as the arena has zones (`min(teams, zoneCount)`). The flag is a
`PickupKind::Flag` pickup whose `spawner` byte holds its team (and so its home zone): no new
pickup field, the snapshot reader's enum check simply admits the third kind. Rule row 52
(`rules/flag.cpp`, `Player[!Carrying] × Pickup[Flag]`) takes it for a monkey of another team and
calls `onPickupTaken`, where capture sets `Tag::Carrying` and records the carrier; its own team's
touch declines, so a dropped flag goes home only by the clock (no touch-to-return). Scoring is
judged in `onPlaying`, after the interactions, against the carrier's centre; the drop happens in
`onEliminated` (the victim is still in the pool, so its position is known; a pit drops the flag
into the pit, where the clock returns it). A flag dropped on tick T goes home on tick
T + `flagReturnTicks`; a pickup pool too full to lay a flag sends it home, or retries the home
flag on the next Playing tick. Carrying changes no stat and bananas treat a carrier like anyone.
Both kinds are team kinds (`teams` >= 2) and respawning kinds (`score_to_win`, `respawn_ticks` >=
1); `sim::scoresPoints(kind)` tells the hosts their scores are points, not round wins. The scripted
input generator walks to the objective in these two kinds only (hill: zone 0 while outside it;
capture: a carrier home, anyone else the nearest flag of another team), from state alone with no
Rng draw, so every other mode's inputs are unchanged.

As built (v0.34, step 4: the grid). The space is a pair of hooks, not a system: `InputSystem`
decodes every alive monkey's `Intent` as before and then calls `spaceBehaviorFor(mode.space)
.latch(s, content, frame)`; `MovementSystem` is `spaceBehaviorFor(mode.space).move(s,
content)`. The order of the ten systems is unchanged, and so is everything around the hooks:
`AbilitySystem` runs between the latch and the move (so a press on a latch tick activates
after the latch read the frame, and a dash that starts on that tick replaces the walk the latch
committed), and `CollisionSystem` sweeps whatever `prevPos..pos` the move produced. The
continuous row is the facing line `InputSystem` had (`facing = aim` every tick) and the movement
body, byte for byte: the gate passed unchanged over the refactor before any state field landed.
`InputSystem` now takes the `MatchContent` (`inputSystem(s, content, frame)`). Rooting
(`rooted(p, content)`, §5.10) is read by both spaces: the continuous move zeroes the stick's
velocity, the grid latch takes no commitment.

### 5.4 Input model

```cpp
struct PlayerInput {            // 5 bytes on the wire, bit-packed smaller
  uint8_t moveDir;              // 0..255 angle, 0 = +x (digital input uses 8 of them)
  uint8_t moveMag;              // analog magnitude; 0 = idle, 255 for digital input
  uint8_t aimDir;               // 0..255 angle (twin-stick aim)
  uint8_t buttons;              // bit0 Throw, bit1 Dash, bit2 Ability1 (slot 3, v0.21), bit3 Ability2, bit4 Taunt ...
  uint8_t subTick;              // stamp of the latest press edge within the tick (1/256 tick ≈ 65 µs)
};
struct InputFrame {
  Tick tick;
  std::array<PlayerInput, kMaxPlayers> inputs;
  PlayerMask present;
};
```

The stick is idle iff `moveMag == 0` (v0.13); `moveDir` is always an angle, so every one of the
256 directions, +x included, is reachable. This struct is also the bot action space (§8). Button presses are
edge-detected in `InputSystem`, which makes "repeat last input" a safe prediction for remote
players: a held button never re-fires. `subTick` is explained in §5.11. Precisely (v0.14): it is
the sub-tick stamp of the latest press (0 → 1) edge of any button within this tick; releases
never update it, so it only carries meaning on a tick where a press edge occurred and
repeated-input prediction is unaffected. The input sampler that fills it arrives in M2.

Roster (v0.17): `InputFrame::present` is the authoritative roster. `InputSystem` copies it into
`MatchRules::present` each tick; `RulesSystem` spawns the present slots and despawns any alive
player whose slot is no longer present when a round starts. A slot that joins mid-round therefore
waits for the next round (its bit counts toward the two-player minimum at once), and a slot that
leaves mid-round keeps its idle monkey until it is eliminated or the round ends. In deathmatch
(v0.23), whose match is one round, a joiner spawns `respawnTicks` after it appears and a leaver's
eliminated monkey does not respawn (§5.3). The server
changes the mask only through its frames, so prediction, rollback and replays need nothing else.

Teams (v0.32): `MatchConfig::teams` is `TeamSet`, a team index per slot below `kMaxPlayers`,
constant for the match (not in `InputFrame`, not in `WorldState`); the default
`kFreeForAllTeams` gives slot i team i. Hosts fill it with `sim::teamsFor(mode, present)` =
`balancedTeams(ModeDef::teams, present)`: the present slots in slot order round-robin over the
teams, then every other slot in slot order to the team with the fewest slots so far, so a slot
that joins during the match lands on the smallest team as it stood at the start. Ten bytes in
the replay header, four bits a slot on the wire. The local lobby recomputes it from the joined
slots and bots whenever a match starts or restarts; `kfm_server` at every restart from the
slots present then; the command-line hosts from the mode through `resolveMatchNames`.

Input sampler (v0.15, `engine/platform`, SDL-free and unit-tested headlessly): the client
translates OS events into a platform-neutral `InputEvent` (key, pad button, pad axis, pad
added/removed, with the OS timestamp on the `SDL_GetTicksNS` clock) and pushes them into an
`InputSampler`, which is sampled once per tick immediately before `step()`:
- Actions, not keys: Up/Down/Left/Right/Throw/Dash, each with key aliases per keyboard profile
  and button aliases per gamepad profile. An action is down while any alias is down, so a
  second alias of a held action is not an edge; key repeats are ignored.
- Tap semantics: a button bit is set if the action is held at sampling time or was pressed
  during the tick, so a tap shorter than one tick yields exactly one pressed tick followed by a
  released one. Each tick samples the key and button events stamped inside its wall-clock
  window: a frame that runs several steps gives each press to the tick whose window contains
  it (later events stay pending for the next tick of the batch), and the batch's last tick,
  which ends at the frame's sample time, takes everything still pending. "Held" means held at
  the end of the tick's window; stick axes are state and are read at sampling time.
- `subTick = clamp((pressNs − tickStartNs) · 256 / tickDurationNs, 0, 255)` for the latest
  Throw or Dash press edge of the tick; movement keys and releases never stamp; 0 without a
  press edge; late presses clamp to 0 and presses after the tick end to 255.
- Movement: digital keys give the 8 directions with `moveMag = 255` and opposing keys cancel;
  sticks use a radial deadzone of 0.2 rescaled so the deadzone edge is 0, SDL's +Y-down flipped
  to sim +y-up, `moveMag` in 1..255 outside the deadzone and exactly 0 inside; the dpad is used
  only while the left stick is inside its deadzone.
- Aim: the right stick when outside its deadzone, otherwise the current non-idle move
  direction, otherwise the previous aim (never reset). Keyboard aim follows movement.
- Local multiplayer: a device feeds at most one slot; `PadRemoved` unbinds its slot and flags it
  as dropped (nothing rebinds automatically); `pollJoin()` reports the first unbound device
  that pressed Throw or Dash this tick, which is how "press a button to join" works;
  `consumeButtons()` then marks the slot's presses so far and its held Throw/Dash as used until
  released, so the joining press never reaches the sim as a press edge. Default
  bindings: keyboard profile 0 is WASD with J or Space to throw and K or Left Shift to dash;
  profile 1 is the arrow keys with Period or Right Ctrl to throw and Slash or Right Shift to
  dash; gamepads move with the left stick (dpad fallback), aim with the right stick, throw with
  West or Right Shoulder and dash with South or Left Shoulder.
- Ability button (v0.21): a third action, `Action::Ability`, sets `Button::Ability1` (bit 2,
  the third ability slot) with the same tap, held, sub-tick, join and consume semantics as
  Throw and Dash. Default bindings: L or E (keyboard profile 0), Semicolon or Apostrophe
  (profile 1), North or East (gamepads). `PlayerInput` stays 5 bytes: the bit already existed.
  The in-game controls help lists it; the browser page needs nothing (SDL maps the keys).
- Keyboard + mouse (v0.45): the mouse belongs to keyboard profile 0 (one device; the lobby card
  says "Keyboard + mouse"), whose `KeyboardProfile::mouseAim` is set. The mouse buttons are keys
  (`Key::MouseLeft`/`MouseMiddle`/`MouseRight`, sent as `MouseButtonDown`/`Up` with the OS
  timestamp), so they alias actions like any key: profile 0 throws with J, Space or the left
  button and dashes with K, Left Shift or the right button (the middle one binds nothing), and a
  click on a free keyboard side joins it. Aim: while mouse aim is on, a mouse slot's aim is the
  direction from its aim origin to the cursor's ground point, both sim-space floats the host sets
  before every sampled tick (`setAimOrigin`, `setCursorGround`), since the sampler knows no world
  and platform never sees the camera; the move direction never aims then, and an unknown point
  or a cursor within 0.05 units of the origin keeps the previous aim (never 0). Mouse aim turns on
  with a mouse move or click and off when the cursor leaves the window (`MouseLeave`), so a
  keyboard-only player on profile 0 who never touches the mouse aims with movement as before.
  Mouse aim is also a player setting (v0.55: Settings > Mouse aim, on by default; `--mouse-aim
  on|off`, `?mouse_aim=`, saved `mouse_aim=on|off`). Off, the profile aims by its move direction
  exactly as before v0.45 while left and right click still throw and dash; the switch is
  `InputSampler::setMouseAimEnabled`, and flipping it either way drops the "the mouse has moved"
  latch, so turned back on the cursor aims only once the mouse moves or clicks again. It is
  client-side input shaping only: the sim and replays are unchanged.
  The host side is `feedMouseAim` (apps/client): the cursor (window coordinates × the pixel
  density) through `screenToGround` of the arena camera, and the slot's monkey in the world the
  tick steps: `MatchSession`'s current world, `NetClient::world()` online (the predicted world,
  never the confirmed one, which trails it by the lead). Mouse events synthesized from touch or
  pen are dropped, so touch is unaffected. A mouse button press while ImGui wants the mouse (a
  menu, the lobby panel, the match-over buttons, the F1 overlay) stays out of the game, like a
  key press while a text field has the keyboard; releases always go through, and the sampler
  ignores a release it saw no press for. No pointer lock: the cursor stays visible; the browser
  page's canvas cancels `contextmenu`, so the right button never opens the page's menu. No
  mouse-aim setting: the settings model would need a key, a flag, a URL parameter and a row for
  it, and leaving the window already hands aim back to the keys.

Grid space (v0.29, §5.12): `PlayerInput` is unchanged. In a grid mode `InputSystem` latches the
frame once per turn, on the Playing tick where `ticksInPhase % turnTicks == 0`: the latched
`moveDir` (quantised to the nearest of the eight neighbours, idle when `moveMag == 0`) is the
cell the monkey commits to for the turn. Presses are still edge-detected every tick, so a tap
inside a turn is not lost, and the `subTick` of the latch tick breaks ties for a contested cell.
Facing follows `aimDir` quantised to eight directions at the latch; the SDKs set aim to move by
default, so facing follows movement unless the bot or player aims. On a one-tick turn a bot
sees no difference from the continuous mode.

As built (v0.34, `src/spaces/grid.cpp`, `kfm/sim/spaces/grid_cells.hpp`): the latch reads the
frame's `moveDir`, `moveMag` and `subTick` directly (an absent slot is idle), so it depends only
on the world state (`ticksInPhase`, positions, commitments, ability phases, tags) and the frame,
and rollback over a latch is exact like any tick. The octant is `((moveDir + 16) / 32) mod 8`:
octant 0 is +x, counter-clockwise, a half-way direction (16, 48, ...) goes to the
counter-clockwise octant; a unit test checks all 256 values against a brute-force nearest
direction, and against the dash's path (`angleOf` of the decoded unit vector, with eight BAM of
slack for its rounding). Facing: outside Playing it follows aim every tick as in the continuous
space (so the countdown shows the stick); during Playing it is set to the aim at each latch,
rooted or not, and held in between. The aim the throw reads (`Intent::aim`) is still the
current tick's, so rotation never costs anything (§5.12).

### 5.5 Abilities (the modularity core)

- `AbilityDef` (content): `kind`, cooldown, phase durations (startup / active / recovery),
  numeric params, and the `Tags` granted during each phase.
- `AbilityState` (per slot, POD): `defIndex`, `phase`, `phaseTicks`, `cooldownTicks`, and the
  activation record `activatedTick`, `activatedSubTick` (press time, read by the catch rule and
  the tie-break) and `dir` (dash or throw direction captured at activation). There is no
  kind-specific scratch union; a kind that needs more state adds a named field (v0.14).
- `AbilityBehavior` registry: a table indexed by `AbilityKind` holding function pointers
  `canActivate`, `onActivate`, `onTick`, `onPhaseChange`, `onEnd` and (v0.21) `kindTags`, the
  tags the kind itself grants from the slot's state on top of the def's phase tags (Dash:
  `PerfectDash`); `AbilitySystem` re-derives every bit in `kAbilityKindTags` each tick. v0.48:
  `holdsPhase(state, def, buttonHeld)` (default `neverHolds`), pure like `kindTags`: true keeps
  the current phase going past its duration, `buttonHeld` being the slot's button in this
  tick's intent; `phaseTicks` saturates at 65535, so a phase held for good never wraps. The table is a `constexpr`
  array (no startup registration, no mutable static state). Adding an ability touches a new behaviour file, a content def, one registry line and
  a scenario test. Every hook is called unconditionally, so the registry `static_assert`s that
  every row (one per `AbilityKind::kCount`) has every hook set, with the hook count pinned by
  `fieldCount<AbilityBehavior>()` (v0.70): a forgotten row or hook is a compile error, not a
  null call at the first dispatch. The mode (`ModeBehavior`, §5.3) and space
  (`SpaceBehavior`, §5.12) registries assert the same.
- Players have N ability slots. Base kit is {Dash, Throw}. Buttons belong to slots, not to
  kinds (v0.21, `slotButton()`): slot 0 fires on `Button::Dash`, slot 1 on `Button::Throw`,
  whatever kind the slot runs. The catch rule and the tie-break (§5.11) read the press time of
  the slot that grants the tag they key on (`slotGranting()`), the dash slot in the base kit. Power-ups may override a slot
  (for example a teleport dash) or add a temporary slot.
- Phase tags: Dash's active phase grants `Tag::Dashing`; its first `perfectWindowTicks` also
  grant `Tag::PerfectDash`, plus the tick after them when the press had a non-zero `subTick`,
  because the window `[press, press + perfectWindowTicks)` measured from the press instant
  closes inside that tick (§5.11). The tag says "the window may be open this tick"; the catch
  rule judges the exact instant. Tags are a bitset read by the interaction rules.
  As built (v0.34): a window of 0 ticks never grants `PerfectDash`, the sub-tick tail
  included (before, a press with a non-zero sub-tick opened the tail tick of an empty
  window), so the grid's base dash never catches; and a window may equal the active phase
  (the catcher's): the tail tick then falls in the recovery, where no phase grants the tag,
  so the window closes with the active phase. Validation: a dash needs `active_ticks >= 1` and
  `perfect_window_ticks <= active_ticks` (was: strictly less).
- The charged throw (v0.48, `throw.cpp`): a Throw def with a positive `speed` charges.
  `param0` is the banana speed multiplier of a tap, `param1` that of a full charge and `speed`
  the multiplier gained per tick held past the startup (TOML `charge_min`, `charge_max`,
  `charge_ticks`; the loader stores `(max − min) / ticks`, truncated, and requires
  `0 < min < max`). Its `holdsPhase` keeps the Startup going while the button is down, so a
  tap still throws on the second tick after the press and a hold throws on the first tick the
  button is up. On the Startup → Active transition the hold is measured from the press
  (`tick − activatedTick − startupTicks`, since `settle` has reset `phaseTicks`), the
  multiplier `min(param0 + speed × held, param1)` joins the power-ups' `speedMul`, and the
  direction is the aim of the release tick (an uncharged throw keeps the press's aim). The
  cooldown still counts from the press. Rejected: scaling `outboundTicks` per banana, which
  changes the flight rule every bot reproduces and needs a per-banana field in the
  observation (a new Observation version); a hold that slows or roots the thrower (not asked
  for; content could add it with `root_phases`). Shipped: ×0.6 to ×1.5 over 40 ticks (5.8 to
  14.4 units out), Fast Hands over 20; the grid throws do not charge.
- Kinds beyond the base kit (v0.21), each one behaviour file under `src/abilities/`, a def in
  `content/abilities.toml`, one registry line and a scenario test; they read only `AbilityDef`
  numbers and add no `AbilityState` field:
  - Blink (`blink.cpp`): the direction is captured at the press (stick, else facing); on the
    Startup → Active transition the monkey moves by `dir × speed × activeTicks` through
    `statics::moveCircle` (Slide, outer box included), so walls stop it like a walk. The jump
    happens in `AbilitySystem`, before `MovementSystem` sets `prevPos`, so the swept collision
    never sees the path: bananas and monkeys on it are passed, not touched. Blink grants no
    tags. Decided against `Dashing` during the startup: that tag arms the dash attack (row 25),
    so a standing blinker would strike whoever walked into it; without it the startup is a
    plain, readable telegraph during which a banana hits as usual, and nothing in the rules
    table changes. `PlayerBlinked` carries the origin. Shipped: 6-tick startup, a 5-unit jump
    (30 u/s over 10 ticks), 1.5 s cooldown, in loadout `blink` (mode `rounds_blink` until v0.31).
  - Slam (`slam.cpp`, `rules/slam.cpp`): the behaviour only marks the impact. `kindTags`
    grants `Tag::Slamming` on the first Active tick only (phaseTicks 0), so a slam acts once
    however long its active phase; the Startup → Active transition emits `SlamLanded`. While a
    player is `Slamming`, `CollisionSystem` uses the slam def's `param0` (the reach, from the
    slammer's centre) as its contact radius, so the broadphase finds what the slam reaches; the
    rules table does the rest (rows 16 and 26, §5.6). A pushed monkey gets
    `Player::knockback = away × param1 / activeTicks` and `knockbackTicks = activeTicks`;
    `MovementSystem` adds the knockback to whatever velocity it computed (stick, dash, stun)
    and counts it down, its only change. The slammer walks during the startup (no `Dashing`,
    as for Blink). Shipped: 0.2 s startup, reach 3, push 4 units over 0.2 s, 2 s cooldown, in
    loadout `slam` (mode `rounds_slam` until v0.31). New state: `Player::knockback`, `knockbackTicks` (visited).
  - Parry (`parry.cpp`, `rules/parry.cpp`): the window is `[press + startupTicks, press +
    startupTicks + activeTicks)` from the press instant (`activatedTick + activatedSubTick /
    256`), like the perfect dash's. `kindTags` grants `Tag::Parrying` on the Active ticks plus
    the first Recovery tick when the press had a non-zero sub-tick (the window closes inside
    it; the loader requires `recovery_ticks >= 1`); the rule judges the exact instant (§5.6).
    `PlayerParryStarted` on the press (the stance effect). Shipped: 2-tick startup, an 8-tick
    window, 14 ticks of recovery (a whiff is punishable), 1 s cooldown, in loadout `parry`
    (mode `rounds_parry` until v0.31).
  - Call (`call.cpp`, v0.35): communication through the game (§8). The press emits
    `MonkeyCalled{player, entity = the player, a = the symbol, b = the def's range as raw
    Q16.16}` and nothing else in the world changes but the slot's own cooldown and phases. The
    symbol is the press tick's aim byte (`Intent::aim`), not the facing, which on the grid
    follows the aim only at a latch; a call pressed on a latch tick therefore also turns the
    monkey's facing (and its cone) to the symbol for that turn, the price of talking. TOML
    `kind = "call"` with `range` in units (the def's `param0` under its own name: `range` on
    another kind, or beside `param0`, is an error), validated in (0, 128]. Hearing is the bot
    hosts' (§8, `AgentHost::observe`): every other monkey whose centre lies within `range`
    Chebyshev (the larger of |dx| and |dy|) of the caller's after the step, ally or enemy, so
    on a grid arena cells × `cellSize` is the content author's arithmetic. Shipped:
    `grid_call` (cooldown 15, one call per 15-tick turn) and `grid_call_1t` (cooldown 1),
    range 4 = the grid modes' cone on the courtyard's 1.0 cells, on the ability button of the
    four grid kits. `ObsAbilityKind::Call` (6) is appended; the scripted generator presses a
    call in the third slot one tick in `abilityOneIn`, with its aim as the symbol (no other
    shipped kit has one, so no other mode's inputs change).

### 5.6 Interaction rules (where the twists live)

`CollisionSystem` emits contacts `(a, b, info)`. `InteractionSystem` resolves each by scanning
a prioritised rules table keyed on `(kindA, requiredTagsA, kindB, requiredTagsB)`; the first
match whose handler returns `true` wins. A handler returning `false` means "did not apply, keep
scanning": that is how a perfect catch judged outside its time window falls through to
`DamagePlayer` without the catch rule knowing about damage. Contacts are resolved in `tHit`
order (then earlier dash press, then pool index), and a contact whose entity an earlier contact
despawned is skipped. Every row forbids `Tag::Eliminated` on the player: a player eliminated
earlier in the same tick stays in the pool until `LifecycleSystem` and must not be hit, catch
or pick up again. v0.57: the same set (`kGone`) holds `Tag::Falling`, so no row touches a
monkey falling down a pit either, on whichever side of the pair it is; and every Boomerang row
names an Outbound/Returning phase mask, so no row touches a banana sinking into a pit either.

```
prio  A                          B                              handler
 10   Player[PerfectDash]        Boomerang[InFlight]            CatchBoomerang (declines at ammoMax)
 15   Player[Parrying, !HitStun] Boomerang[InFlight, !own]      ParryBoomerang (declines outside the window)
 16   Player[Slamming, !HitStun] Boomerang[InFlight, !own] (reach) SlamGroundsBanana
 20   Player[Shielded, !HitStun] Boomerang[InFlight, !own]      ShieldBlocksBanana
 24   Player[Dashing, !HitStun]  Player[Shielded, !HitStun]     ShieldBlocksDash (declines below minAmmo)
 25   Player[Dashing, !HitStun]  Player[!HitStun, !Invulnerable] DashAttack (declines below minAmmo)
 26   Player[Slamming, !HitStun] Player[] (reach)               SlamPushesPlayer
 30   Player[!HitStun]           Boomerang[InFlight, !own]      DamagePlayer
 40   Player[]                   Boomerang[Returning, own]      AutoCatch
 50   Player[]                   Pickup[Banana]                 TakePickup (declines at ammoMax)
 51   Player[]                   Pickup[PowerUp]                GrantPowerUp (declines with a full list)
 52   Player[!Carrying]          Pickup[Flag]                   TakeFlag (v0.33; declines for the flag's own team)
 60   Player[!Dashing]           Hazard[Pit] (whole body)       FallIntoPit (v0.57: starts the fall)
```

Parry (v0.21, row 15, `rules/parry.cpp`). Judged like the catch: parried iff
`0 <= (tick + tHit) − (pressTime + startupTicks) < activeTicks`; outside the window the handler
declines and the scan falls through to the slam, shield and damage rows, so a mistimed parry is
a hit. A parried banana becomes the parrier's (owner swap), `Outbound` with a fresh
`outboundTicks` timer, placed at the contact point and heading for the thrower's current
position (straight back when the thrower is gone), at its own speed with its projectile
modifiers kept; `BoomerangParried{parrier, banana, a = thrower}`. As the parrier's own outbound
banana it passes through the parrier, may hit the thrower (credited to the parrier) and returns
to the parrier, so a parry gains a banana. Order: below the perfect catch (rows 10/11) because
both reward timing and the catch also puts the banana in hand at once, so it wins a tie (a
dasher at full ammo declines the catch and the parry takes it); above the slam, shield and
damage rows, which it pre-empts the way the catch pre-empts damage. It ignores own bananas: a
returning own banana is still auto-caught (row 40).

Slam (v0.21, rows 16 and 26, `rules/slam.cpp`). A contact made only through a slammer's widened
radius (§5.3, §5.5) carries `Contact::reach` (`kReachA`/`kReachB`, swapped with the orientation)
and matches only a row with `reachA` set, from the slammer's side: the reach never catches,
damages, picks up, dash-attacks or blocks. Row 16 grounds a non-own banana in flight where it
is (it becomes a pickup like any grounded banana); it sits below the catch rows (a perfect catch
still gains the banana) and the parry, and above the shield and damage rows, so a banana that
reaches the slammer's body on the impact tick is knocked down too. Row 26 pushes any monkey that
is not eliminated (a push is no hit: shields and spawn protection ignore it) and emits
`PlayerSlammed`; it sits after the dash attack, so a dasher's attack on the slammer at the same
`tHit` lands first. A stunned slammer (`HitStun` earlier in the tick) slams nothing. Both
require `Slamming`, which only the impact tick grants: before it a banana hits the slammer as
usual (the fall-through).

M1 ships rows 10/11 (the catch, see below), 30, 40 and 50 (`takePickup`: a grounded banana
restores one ammo and stays put at `ammoMax`); v0.16 adds row 25 (the dash attack, below) and
v0.22 row 60 (the pit, `rules/hazard.cpp`); v0.20 adds rows 20, 24 and 51 (§5.8) and the row
field `pickupMaskB` (allowed `PickupKind` bits when B is a pickup, like `phaseMaskB` for a
banana) that splits row 50 from row 51. Walls are not in the
table (v0.22): interior walls and the outer box are resolved while integrating, players sliding
in `MovementSystem` and bananas bouncing in `ProjectileSystem`, through one function
(`statics::moveCircle`, §5.10), so a wall never produces a contact. A `Boomerang × Wall` row
would come back only when some banana kind should not bounce; it would then be a
`MoveParams::response` choice made from the banana's modifiers rather than a contact.

Relation (v0.29). Every row carries a `relationB` mask over {Self, Ally, Enemy}, judged between
A and B's owner (B itself for a player pair) through `MatchContent::relation`; the default is
every bit, and `Ownership` stays the own-banana test it is. A free-for-all config makes every
other slot an enemy, so with these two rows the table plays bit for bit as before:

```
prio  A                               B                                    handler
 30   Player[!HitStun]                Boomerang[InFlight, !own] (Enemy)    DamagePlayer
 31   Player[!HitStun, FriendlyFire]  Boomerang[InFlight, !own] (Ally)     DamagePlayer
```

Friendly fire is therefore a tag the mode's baseline grants (§5.3), not a flag a handler reads;
the dash attack and the slam push get the same pair of rows when a team mode wants them.

As built (v0.32). `InteractionRule::relationB` is a mask over `relationBit(Relation::Self |
Ally | Enemy)`, default `kRelationAny`; `InteractionSystem` judges it after the tag checks,
between A and B (a player pair, either orientation) or B's owner (a banana), and passes any
mask for a pickup or hazard. An owner slot that is invalid counts as an enemy, as every other
slot is in a free-for-all. Every row that hurts, pushes or answers a hit applies to an Enemy
and has an Ally twin that additionally requires `Tag::FriendlyFire` on the monkey on the
receiving end (A for a banana row, B for a player pair), the rest of the row unchanged. The
twins sit a few priorities below the Enemy rows; a contact is either Ally or Enemy, so a twin
only ever competes with its own relation's rows, in the same order:

```
prio  A                                   B                                       relation
 10   Player[PerfectDash]                 Boomerang[InFlight, !own]               any (a pass)
 11   Player[PerfectDash]                 Boomerang[Returning, own]               any
 15   Player[Parrying, !HitStun]          Boomerang[InFlight, !own]               Enemy
 16   Player[Slamming, !HitStun]          Boomerang[InFlight, !own] (reach)       Enemy
 17   Player[Parrying, FF, !HitStun]      Boomerang[InFlight, !own]               Ally
 18   Player[Slamming, FF, !HitStun]      Boomerang[InFlight, !own] (reach)       Ally
 20   Player[Shielded, !HitStun]          Boomerang[InFlight, !own]               Enemy
 21   Player[Shielded, FF, !HitStun]      Boomerang[InFlight, !own]               Ally
 24   Player[Dashing, !HitStun]           Player[Shielded, !HitStun, !Invuln]     Enemy
 25   Player[Dashing, !HitStun]           Player[!HitStun, !Invuln]               Enemy
 26   Player[Slamming, !HitStun]          Player[] (reach)                        Enemy
 27   Player[Dashing, !HitStun]           Player[Shielded, FF, !HitStun, !Invuln] Ally
 28   Player[Dashing, !HitStun]           Player[FF, !HitStun, !Invuln]           Ally
 29   Player[Slamming, !HitStun]          Player[FF] (reach)                      Ally
 30   Player[!HitStun, !Invuln]           Boomerang[InFlight, !own]               Enemy
 31   Player[FF, !HitStun, !Invuln]       Boomerang[InFlight, !own]               Ally
 40, 50, 51, 60 as before                                                          any
```

Decided beyond the v0.29 sketch: the parry, the slam's grounding and both shield rows get twins
too, because each pre-empts a row that has one (a shield breaks only on a hit that would land;
a parry or a slam would otherwise steal or knock down a teammate's banana that passes through
harmlessly). The perfect catch keeps every relation: catching a teammate's banana is a pass.
`Tag::FriendlyFire` is bit 8 (nine of 32 used), grantable by a power-up's `tag` grant
(`friendly_fire`) and by nothing else. A test pins every twin against its Enemy row field by
field. In a free-for-all no Ally row can match and every Enemy row matches what the row matched
before, so the gate's checksums did not move.
Contested cells on the grid are not rows: they are resolved before anyone moves (§5.12). The
catch stays row 10 in every space; what changes on the grid is who can ever be `PerfectDash`
(§5.12).

Pit (v0.22): `CollisionSystem` emits `Player × Hazard` for every hazard holding a player's
centre at the end of the tick (`EntityKind::Hazard`, `b.index` = the hazard's index in
`content.arena.hazards`, `tHit` = the centre's entry time). v0.57: for pits, one contact for the
lot and only once the player's whole contact disc (the radius stat) lies inside the union of
the pit boxes (`discOverPits`), naming the first pit box holding the centre and timed at the
end of the tick (`tHit` = 1), where the test was made, so every other contact of the tick (a
banana that reached the monkey first) resolves before it.

Pit falls (v0.57). Row 60 forbids `kGone` (`Eliminated`, `Falling`) and `Dashing`: no other
tag (not `Invulnerable`, not a shield) saves a monkey wholly over a pit, but a dash carries it
across, so the widest pit a monkey clears is the dash's reach and nothing measures pits. The
test is the whole body: the monkey's closed contact disc inside the union of the closed pit
boxes (`discWithinBoxes` in `content/arena.hpp`; touching the edge from inside counts, a sliver
on the floor does not). It is the union, not each box, because a tilemap pit is merged into
several boxes and a monkey straddling two adjoining ones is over the hole. The test is exact
and integer-only: the disc's bounding square is cut at every pit edge that crosses it into
sub-rectangles no edge enters, each one is wholly inside or outside every box, and the disc is
over the pits iff every sub-rectangle it overlaps has its (doubled) midpoint in some pit box. At
most 34 cuts per axis, usually none, so it costs a few comparisons per monkey per tick; the
reference bot runs the same function over the observation's boxes. A pit narrower than the body
(2 × radius) in its narrow dimension takes nobody. `fallIntoPit` declines for non-pit kinds (a slow patch acts through stats,
§5.8) and otherwise starts the fall: `Tag::Falling`, `Player::fallTicks = PlayerDef::fallTicks`
(TOML `[player] fall_ticks`, 36; at least 1, counting the tick it went in), the velocity, a
slam's pending push and a grid commitment cleared, every ability slot back to Idle without
`onPhaseChange`/`onEnd` (a charging throw is dropped, not thrown), its bananas away from it sent
to the pit (below), and `PlayerFell{player, entity, a = the hazard index}`. While it falls:
- InputSystem hands it an idle intent with the aim at its facing (no press reaches a slot, the
  latch keeps the facing); `prevButtons` still follows the input, so no stale edge fires after.
- The spaces leave it where it went in (velocity zero; the grid drops its commitment and skips
  it at the latch).
- No rule row touches it (`kGone`): bananas fly over it, a dash attack or a slam push misses
  it, it takes no pickup and catches nothing; `zoneOccupants` leaves it out.
- Its bananas: the one in its hand goes down with it (the one exception to "bananas never
  disappear"; a monkey killed any other way drops its held bananas around it). Every one away
  from it (outbound or
  returning) drops where it is the moment the fall begins (`dropBananasOf` in `rules/fall.hpp`:
  Grounded, velocity zero), and `LifecycleSystem` lays it as a floor pickup that very tick, as it
  does for the bananas of any monkey that dies (dropped as it despawns, on the tick of the
  elimination). A banana that comes down wholly over the pits sinks into the hole instead
  (§5.7), so no pickup lies where only a fall could reach it. Round starts and respawns hand out
  `ammo_start` as ever; when no banana is left at all, the dry-round rule (§5.3) ends the round.
- `LifecycleSystem` counts `fallTicks` down in every match phase (after the respawn timers,
  before the despawns) and at 0 eliminates it by nobody (`rules/fall.hpp`, `eliminateFallen`):
  hp 0, `Eliminated`, `PlayerEliminated` with killer 0xFF and no `PlayerHit`, then the mode's
  `onEliminated` with no killer (a deathmatch counts the death and schedules the respawn,
  nobody scores; capture drops a carried flag where the monkey went in). It despawns in the
  same pass. A fall that starts on tick T ends on T + `fallTicks` − 1.
- The round kinds wait for it: `rounds`, `survival` and `team_rounds` end no round while a
  monkey is falling (`anyFalling` in `mode_flow`; survival's clock waits too). The faller is
  still in the pool but already out, so the last monkey standing beside it wins only when the
  fall ends, and a round in which both go is a draw. The respawning kinds play on: a fall is a
  death that comes `fallTicks` later.

Dash attack (v0.16): dashing into an enemy while holding at least `DashAttackDef::minAmmo`
bananas damages it. Row 25, `Player[Dashing, !HitStun, !Eliminated] ×
Player[!HitStun, !Invulnerable, !Eliminated]`, handler `dashAttack` (`rules/dash_attack.cpp`).
- Attacker as A: rows key on `(kindA, kindB)` with the actor as A, so `InteractionSystem` tries
  every Player × Player contact in both orientations (A into B, and B into A, the second with
  `a`/`b` swapped and the normal negated). Each orientation is sorted and resolved like any
  other contact; a pair where neither player dashes matches no row and does nothing, so walking
  into a player, or a dash without ammo, passes through.
- Without enough ammo the handler returns `false`; no other row keys on two players, so nothing
  happens. On success it damages the victim exactly like `DamagePlayer` (shared code: hp loss,
  `HitStun`, `PlayerEliminated` at 0 hp) by `DashAttackDef::damage`, emits `PlayerDashHit`
  instead of `PlayerHit`, spawns no pickup and spends one banana only when
  `DashAttackDef::consumesAmmo` is set (default off: the attacker keeps its banana, so the round's
  banana supply floor holds).
- Two players dashing into each other, both with ammo: both orientations have the same `tHit`,
  so the tie-break decides (§5.11): the earlier dash press, then the lower slot. The loser is
  then `HitStun` or eliminated, and row 25 forbids `HitStun` on the attacker, so it cannot
  retaliate in the same tick. A banana that reaches the dasher at an earlier `tHit` resolves
  first and stops the attack the same way.
- Content switch (§5.10): `minAmmo = 0` would make every dash an attack; the default content
  asserts `minAmmo >= 1`.

A new twist is a new rule and possibly a new tag. Rules are tested in isolation by scripted
scenarios ("spawn banana at x moving v, dash at tick t, expect `BoomerangCaught`").
`DamagePlayer` subtracts the banana's damage from the player's HP and eliminates at or below
zero. Players have `hp`/`maxHp` (a derived stat, so power-ups can change it) and bananas carry
`damage`; the defaults make one hit a kill (`damage >= maxHp`), so the base game plays as
one-hit kills while modes and power-ups (armour, heavy banana) can change the numbers.

HP semantics (v0.10):
- `hp`, `maxHp` and `damage` are small integers counting hits, not fixed-point. Base
  `maxHp = 1`, base `damage = 1`. Stat modifiers on `maxHp` are add-only; a `mul` on an
  integer stat is a content validation error.
- When `maxHp` rises (armour picked up), `hp` rises by the same amount. When it falls (armour
  expires), `hp = min(hp, maxHp)` but never below 1: losing a modifier never eliminates.
  `ModifierSystem` applies this when it recomputes `Stats`.
- `DamagePlayer` emits `PlayerHit`, grants `Tag::HitStun` for `hitStunTicks` (content, a few
  ticks) so a banana overlapping the victim across ticks lands once, and emits
  `PlayerEliminated` if `hp <= 0`. Tick convention (v0.13): `hitStunTicks = N` counts the hit
  tick. A hit on tick T protects against contacts on T..T+N−1 (the tag is set in step 7 and
  `ModifierSystem` counts down in step 8 of the same tick), freezes movement on T+1..T+N−1
  (movement on T already ran), and the next hit can land on T+N. A scenario test pins this. The banana keeps flying
  through its victim (v0.27; since v0.28 there is no switch to make it ground instead).
- Shield and armour stay distinct: `Shielded` is a tag that negates one hit entirely and is
  consumed (rule 20, and rule 24 for a dash attack); armour is `+maxHp`. A mode or power-up picks
  one or stacks both. As built (v0.20): rows 20 and 24 key on the tags of the rows they pre-empt
  (30 and 25), so a shield breaks only on a hit that would land. The block removes the first
  `ModifierList` entry whose def grants `Shielded` (ending that power-up), clears the tag and
  gives the victim `HitStun` for `hitStunTicks` like a hit, without damage; a blocked banana
  grounds at the contact point; a blocked dash stops (the attacker's dash
  slot jumps to Recovery and its dash tags drop), because the rest of the dash would otherwise
  land once the stagger ran out. Both emit `ShieldBroken`.
Whether a perfect dash catches your own banana, or a non-perfect dash grants invulnerability,
are rule-table decisions, not code changes. M1 decides the first: the catch is two rows,
`Player[PerfectDash] × Boomerang[InFlight, !own]` (prio 10) and
`Player[PerfectDash] × Boomerang[Returning, own]` (prio 11). An own outbound banana passes
through its thrower with or without a dash; otherwise a dash (22 u/s) that outruns the banana
it just threw (16 u/s) would catch it straight back.
The catch also needs room for the banana (v0.16): `CatchBoomerang` declines when the catcher
already holds `ammoMax`, checked before the time window. The scan then falls through as for a
mistimed dash: a non-own banana in flight reaches row 30 and hits the full-ammo dasher (and flies
on through it, v0.27), an own returning banana reaches row 40 and is auto-caught (absorbed even
at full ammo, as without a dash). A perfect catch therefore always gains a banana, and the dash
is a defence only for a player who has thrown.

### 5.7 Boomerang model

- State: `owner`, `phase` {Outbound, Returning, Grounded}, `pos`, `vel`, `spin`, `ticksAlive`,
  `damage` (default 1, a one-hit kill against base `maxHp`), and (v0.20) the
  projectile modifiers `speedMul` and `radiusMul` snapshotted at the throw (§5.8; v0.48 also
  the charge multiplier, §5.5): the banana flies at `BoomerangDef::speed × speedMul` out and
  back, so a charged throw reaches `spawnOffset + speed × speedMul × outboundTicks`, and its wall limits and contact radius
  use `BoomerangDef::radius × radiusMul`. Bananas already bounce off every wall without limit, so
  an "extra bounces" modifier has nothing to extend and is not built.
- Flight: outbound for `outboundTicks` or until max range, then steer toward the owner's
  current position with a max turn rate. Bounces off arena walls with restitution. Owner
  eliminated → Grounded → becomes a pickup. Hits a player → flies on unchanged (v0.28: no
  `pierce` switch; before v0.27 a hit grounded it at the contact point).
- v0.57: a dead owner's bananas still away from it drop at once (Grounded where they are,
  on the tick of the death, or as a fall begins), so the "owner eliminated → Grounded" path
  above now applies at the death rather than when the banana next turns for home; they become
  pickups like any grounded banana (in a mode whose `death_bananas` is vanish they are
  despawned instead, `BananaVanished`). One more phase, `Sinking`: `LifecycleSystem` turns a
  grounded banana whose disc lies wholly over the pits (`discOverPits` at the banana's contact
  radius) into a sinking one (`BoomerangFell`) instead of a pickup, and despawns it after
  `pitSinkTicks`; it advances in every match phase and matches no rule row (§5.6 "Pit falls").
- A throw spawns the banana `spawnOffset` (1.0) ahead of the thrower along the aim, clear of
  the thrower's contact radius (0.5 + 0.35), so no throw starts with a contact against its owner.
- Caught by a perfect dash (any player) or by touching its owner while returning → `ammo++`.
- M1 flight (`ProjectileSystem`): constant speed; Outbound for `outboundTicks`, then Returning,
  steering toward the owner's current position by at most `turnRateBam` per tick. A returning
  banana whose owner is gone, or that has been returning for more than `maxReturnTicks` (no
  infinite orbit), grounds where it is; the owner check runs only while Returning, so an
  outbound banana keeps flying after its thrower is eliminated and grounds once it turns back.
  Walls reflect per axis with `restitution` and emit one `BoomerangBounced` per bounce tick,
  also when a corner reflects both axes in the same tick.
- Walls (v0.22): the banana moves by its velocity through `statics::moveCircle` with the
  `Bounce` response (§5.10), the same code path for interior walls and the outer box: a crossed
  wall face mirrors the overshoot and reflects that velocity component times `restitution`, a
  wall corner reflects the velocity about the corner normal, and the outer box keeps the M1
  per-axis reflection bit for bit. Still one `BoomerangBounced` per bounce tick. A throw's
  `spawnOffset` travels the same path from the thrower's centre (interior walls only, the outer
  box stays with `ProjectileSystem` as in M1), so a throw into an adjacent wall bounces back at
  once (plus `BoomerangBounced`) instead of spawning inside or behind it. A returning banana
  keeps homing after a bounce; its 6.6°/tick turn rate means it needs many ticks to turn back
  into the wall, and `maxReturnTicks` grounds one that cannot get home, so there is no extra
  bounce cap (and no new `Boomerang` state). Bananas fly over hazards.
- Bananas fly only while the match phase is `Playing`. During `RoundStarting`, `RoundEnded`
  and `MatchEnded` they are frozen like the players (`prevPos = pos`, timers stop) and
  `InteractionSystem` resolves nothing, so a banana still in the air when the round is decided
  cannot kill the winner or return to its owner during the pause; `RoundEnded` then clears it.

### 5.8 Power-ups and modifiers

- `PowerUpDef` (content): visuals, duration, list of grants: `StatModifier{stat, add, mul}`,
  `AbilityOverride{slot, abilityDef}`, `ProjectileModifier{...}`, `Tag` grants.
- `ModifierList` per player (fixed capacity, e.g. 8) with remaining ticks. `ModifierSystem`
  recomputes derived `Stats` each tick as `base × Π mul + Σ add`, iterating in slot order.
  Integer stats (`maxHp`, `ammoMax`) accept `add` only; `hp` is clamped to the new `maxHp` as
  described in §5.6.
- Slow hazards (v0.22) go through the same derivation: `moveSpeed = base × Π mul × hazard + Σ
  add`, where `hazard` is the product of `speedMul` over the `Slow` hazards holding the player's
  centre after this tick's movement (1 elsewhere, and then not multiplied in at all, so an arena
  without slow patches derives bit-identical stats). Like a modifier, it shapes the next tick's
  movement; `MovementSystem` has no hazard code.
- Pickup entities reference a `PowerUpDef` by index and are spawned by arena spawners.

As built (v0.20; `kfm/sim/content/powerup.hpp`, `systems/modifier.cpp`, `rules/powerup.cpp`,
`rules/shield.cpp`, `systems/lifecycle.cpp`):

- `PowerUpDef{name, durationTicks (>= 1), visual{color, shape}, grants[4], grantCount}` in
  `ContentDB::powerUps` (`kMaxPowerUps` 16, `powerUpAt`, `findPowerUp`). `visual.color` indexes
  the player palette (0–9) and `visual.shape` is `Orb`, `Cube`, `Diamond` or `Coin`; the sim
  never reads either. A `PowerUpGrant` has a `GrantKind` and the fields of its kind:
  - `Stat`: `StatModifier{stat, mul, add}` over `MoveSpeed` (mul and add), `MaxHp` and `AmmoMax`
    (whole `add` only; the loader rejects a `mul`), and (v0.34) `Radius`, the contact radius
    (`Stats::radius`, base `PlayerDef::radius`): `mul` in (0, 4] and `add`, the result clamped to
    [1/64, 8]; without a modifier it is the def's bit for bit (base × 1 + 0 is exact).
  - `AbilityOverride{slot, ability}`: the slot (`dash`, `throw` or, v0.21, `ability`) runs that
    ability def while the power-up is live; the last override in slot order wins. Buttons
    belong to slots (v0.21), so the def may be of any kind, and an override of the empty third
    slot adds an ability for the power-up's duration.
  - `Projectile`: `ProjectileModifier{speedMul, radiusMul, damageAdd}`, each mul in
    (0, 4].
  - `Tag`: `shielded` or `invulnerable` (the others belong to abilities and the damage rules).
- State: a `ModifierList` entry is `ActiveModifier{powerUp, ticksLeft}`; the grants are read
  from the def every tick, never copied. Row 51 (`grantPowerUp`) adds an entry with
  `ticksLeft = durationTicks`; the same power-up again restarts the existing entry's timer in its
  slot (no stacking of one def); a full list (8) declines and the pickup stays. Like the banana
  pickup (row 50) it ignores `HitStun` and dashing.
- `ModifierSystem` per player: (1) drop entries at `ticksLeft == 0`, compacting in slot order;
  (2) `Stats` from `PlayerDef` and every `Stat` grant in slot order (`moveSpeed` never negative,
  `maxHp` in [1, 255], `ammoMax` in [0, 255]); (3) the `hp` clamp of §5.6 and `ammo` capped at
  `ammoMax`; (4) ability overrides; (5) tags: it clears exactly the bits it granted last tick
  (`Player::grantedTags`), sets this tick's grants and records them there, so a tag an ability
  phase or a mode also grants survives the power-up's expiry and vice versa (AbilitySystem
  never clears a bit in `grantedTags`, `LifecycleSystem` keeps `Invulnerable` when spawn
  protection ends while a power-up grants it, and ModifierSystem keeps a bit that an ability
  phase or spawn protection holds now); (6) `HitStun` countdown; (7) `ticksLeft--`. A power-up picked
  on tick T therefore shapes the state after ticks T..T+D−1 and is gone after T+D, and the list,
  the stats and the tags always agree between ticks.
- Ability overrides switch a slot only while it is `Idle` (a running dash or throw finishes on
  the def it started with); on the switch the slot's remaining cooldown is capped at the new
  def's `cooldownTicks` (gaining a faster throw takes effect at once; losing it never lengthens
  a cooldown already running). Expiry restores the loadout's def the same way.
- Ammo: an `AmmoMax` add tops the ammo up by the same amount on every pickup (rule 51), then
  `ModifierSystem` caps it at the new `ammoMax`; on expiry the cap falls back and extra bananas in
  hand are lost (bananas in flight still return and are caught up to the cap).
- Projectile modifiers are combined at the throw (`abilities/throw.cpp`) over the thrower's live
  `Projectile` grants in slot order (muls multiply, `damageAdd` adds onto `BoomerangDef::damage`
  saturating at 255) and stored on the `Boomerang` (`damage`, `speedMul`,
  `radiusMul`), so a banana keeps them after the power-up expires or its thrower is eliminated.
- Spawners: `ArenaDef::pickupSpawners` (`kMaxPickupSpawners` 8): `PickupSpawner{pos,
  powerUpSet (bit i = power-up i, 0 = all), firstTicks, intervalTicks (0 = once), maxAlive}`.
  At the end of `LifecycleSystem`, while `Playing`, a spawner fires when `ticksInPhase ==
  firstTicks + k × intervalTicks`, unless `maxAlive` of its own pickups (`Pickup::spawner`) lie in
  the arena; it draws the def uniformly from its set with one `rng.below()` of the world RNG and
  emits `PickupSpawned`. Its timeline is a function of the round clock, so a spawner has no state
  of its own; round start clears pickups. The loader keeps the arena's `maxAlive` sum at or below
  half the pickup pool so grounded bananas always find room. An arena without spawners never
  touches the RNG here (the default `dojo` has none, so the default match plays exactly as
  before). Per match (v0.24) `MatchConfig::powerUps` (bit i = power-up i, `kAllPowerUps` by
  default, carried to the systems in `MatchContent::powerUps`) narrows every spawner's set: the
  def is drawn from the intersection, and a spawner whose intersection is empty skips its beat
  without touching the RNG. The lobby's power-up toggles and `--powerups` set it.
- Shield: rows 20 and 24, §5.6.
- The shipped set (`content/powerups.toml`), picked up in `dojo_powerups` (the dojo plus four
  spawners near the corners, first at 1 s and 4 s into a round, then every 8 s, one alive each):

  | name | duration | grants |
  |---|---|---|
  | `speed_boost` | 8 s | `move_speed` × 1.5 |
  | `big_banana` | 10 s | bananas radius × 2, speed × 0.85 |
  | `triple_banana` | 15 s | `ammo_max` + 2 (tops up 2 bananas) |
  | `shield` | 15 s | `shielded`: absorbs one hit |
  | `fast_hands` | 10 s | throw slot runs `throw_fast` (cooldown 8 instead of 20, startup 1) |

  Fast Hands is an `AbilityOverride` rather than a cooldown stat: a new def needs no new
  `StatKind` and no special case in `AbilitySystem`.
- A mode's baseline (v0.32, `ModeDef::baseline`): the convention is a permanent entry, not a
  slot of its own. `resetPlayer` (every round-start spawn and every respawn) ends with
  `grantBaseline`, which, for a mode with a baseline only, makes the list `{baseline,
  ticksLeft = kPermanentModifierTicks (0xFFFF)}` and applies it at once the way
  `ModifierSystem` would (stats with hp at the new `maxHp`, ammo topped up like a pickup,
  ability overrides, tags recorded in `grantedTags`), so a respawned monkey is covered on its
  first tick rather than after the next `ModifierSystem` pass. `ModifierSystem` never counts a
  permanent entry down (so never drops it); a pickup of the same power-up leaves it permanent
  and only adds its ammo top-up; a shield block may still remove it like any shield. Power-up
  durations are capped at 65534 (loader and `validateContent`) so no pickup is ever permanent.
  A mode without a baseline never takes the new branch, which kept the free-for-all
  checksums. The shipped team modes have none (no friendly fire); a friendly-fire baseline is a
  power-up `grants = [{ kind = "tag", tag = "friendly_fire" }]`, tested in code but not shipped,
  because any new power-up would join every spawner set that names none and change the draws
  of `dojo_powerups` (follow-up: a non-spawnable power-up flag).

### 5.9 Events

- `SimEvent` is a POD tagged union written to a per-tick buffer: `BoomerangThrown`,
  `BoomerangCaught{catcher, how}`, `PlayerDashed`, `PlayerHit{victim, attacker, damage, hpAfter}`,
  `PlayerDashHit{victim, attacker, hpAfter}` (v0.16: a dash attack; `entity` is the attacking
  player where `PlayerHit` carries the banana, and presentation shows both the same way),
  `PlayerEliminated{victim, killer}`, `PlayerFell{victim, entity, a = the pit's hazard index}`
  (v0.57: the start of a pit fall; its `PlayerEliminated` with killer 0xFF follows
  `fallTicks − 1` ticks later, §5.6), `BoomerangFell{owner, banana}` (v0.57: a banana came
  down wholly over a pit and drops into the hole, `Sinking`, instead of becoming a pickup),
  `BananaDropped{monkey, pickup}` (v0.57: a held banana dropped to the floor at a death, a catch
  past `ammoMax` or a spent dash-attack banana), `BananaVanished{owner, banana}` (v0.57: a
  dead or falling monkey's banana in flight went with it, `death_bananas = "vanish"`),
  `BananaRegrown{player, entity}` (v0.57: an empty hand grew a banana, `banana_regrow_ticks`),
  `RoundEnded{winner}`, `MatchEnded{winner}` (an invalid winner is a draw),
  ... v0.23 (modes): `ScoreChanged{player, score}` (a deathmatch elimination scored; round wins
  are carried by `RoundEnded`; v0.57: `a` is the signed score after as int32 bits and `b` a
  `ScoreReason`: Objective, Catch, Suicide), `PlayerRespawned{player, entity, spawn point}` (a deathmatch
  respawn or late join; `PlayerSpawned` stays the round-start spawn), `TimerWarning{seconds}`
  (10, 3, 2, 1 s left on the mode clock).
  v0.21 (abilities): `PlayerBlinked{player, entity = the player, a/b = the origin's raw x/y}`,
  `SlamLanded{player = the slammer, entity, a = the reach's raw value}` (the impact, for the
  shockwave), `PlayerSlammed{player = the pushed monkey, entity = the slammer, a = its slot}`,
  `PlayerParryStarted{player, entity}` (the press), `BoomerangParried{player = the parrier, now
  the owner, entity = the banana, a = the thrower's slot}`.
  `RoundEnded`, ...
- v0.20: `PickupSpawned` and `PickupTaken` carry the pickup's `PickupKind` in `a` and, for a
  power-up, its def index in `b` (a banana pickup's payload stays 0/0), which is all a
  `PowerUpPicked` would carry, so there is no separate event. `ShieldBroken{victim, entity = the
  banana or the dashing player, a = attacker slot, b = the power-up whose shield broke}` (§5.6).
  No event marks a power-up's expiry: presentation reads the `ModifierList` timers from state.
- v0.33 (objectives): `HillHolderChanged{a = the team now holding zone 0 alone, or 0xFF when
  it is contested or empty, b = the zone}`, pushed by `hill` when the sole holder changes (the
  match starts with nobody). A capture flag uses the pickup events: `PickupSpawned` and
  `PickupTaken` carry `a = Flag` and `b = the flag's team` (laid at the round start, dropped,
  sent home), and a capture is a `ScoreChanged` of the carrier. Presentation reads
  `Tag::Carrying` from state; there is no separate flag event.
- v0.35 (the squad contest): `MonkeyCalled{player = the caller, entity = its player, a = the
  symbol (its aim byte), b = the call's range, raw Q16.16}`. Like every event it is no state:
  replays and rollback re-emit it from the inputs, the client journal dedups it; the bot hosts
  fold it into the listeners' observations (§8), the presentation draws a shout and pings a
  local human's own call (§7).
- Consumers: presentation (VFX/SFX), scoring, the match runner's result line, telemetry.
- Under rollback, presentation keys events by `(tick, hash)`. Events re-emitted during
  re-simulation are deduplicated; events from an abandoned timeline that have not played yet
  are dropped; events for ticks at or before the confirmed tick are final.
- `EventJournal` (v0.17, `kfm/net/event_journal.hpp`) implements this for `NetClient`: 2048
  entries in a fixed array sorted by tick, the hash over (kind, player, entity, a, b), equal
  events of one tick matched by multiplicity. An event the host already drained that a
  re-simulation does not emit again is counted as retracted (presentation cannot un-play it).
  Final means at or before the client's verified tick, or older than the ring (no rollback can
  reach it); delivered final entries are forgotten.

### 5.10 Arenas and content

- Arena: static collision shapes, spawn points, pickup spawners, hazards, in the immutable
  `ArenaDef`. As built (v0.22), see "Arena statics" below: axis-aligned boxes only (convex
  polygons dropped until an arena needs one) and no prebuilt spatial hash (a plain loop is
  cheap at ≤ 32 walls).
- Arena statics (v0.22, `kfm/sim/content/arena.hpp`, `engine/sim/src/systems/statics.hpp`):
  - `ArenaDef` = `name`, `halfExtents` (the outer box), `spawnPoints`, `walls[kMaxArenaWalls =
    32]` + `wallCount`, `hazards[kMaxArenaHazards = 16]` + `hazardCount` (pickup spawners for
    power-ups get their own block). `Wall{min, max}` is a solid axis-aligned box;
    `Hazard{min, max, kind, speedMul}` with `HazardKind::Pit` and `Slow` acts on a player whose
    centre is in the closed box and never blocks anything. All of it is visited, so it is part
    of the content hash; unused slots stay zero.
  - One code path for circles against statics, `statics::moveCircle(arena, from, delta, vel,
    {radius, response, restitution, outerBox})`: (1) x moves by `delta.x`; among the walls whose
    face band `[min.y, max.y]` holds the circle's y, the nearest face it crosses (start on or
    outside `min.x − r`, end past it, so nothing tunnels at any speed) stops it (`Slide`: at the
    face, `vel.x = 0`) or reflects it (`Bounce`: overshoot mirrored, `vel.x = −vel.x ×
    restitution`); (2) rounded corners: every wall the circle still overlaps (only a corner
    zone can be entered this way, never the box) pushes it out along the normal from the
    closest box point to exactly `r`, removing (`Slide`) or reflecting with restitution
    (`Bounce`) the inward velocity; a centre found inside a box leaves through the nearest
    face; (3) y the same way, then corners again; (4) the outer box: `Slide` clamps per axis to
    `halfExtents − r` and `Bounce` mirrors per axis, the M1 code unchanged. With no walls the
    function is exactly M1's `pos += vel` and clamp/reflect, so the default match keeps its
    checksums. Players use `Slide` with their velocity (stick or dash: a dash stops at a wall
    and its phases and perfect window still run their ticks); bananas `Bounce`, and a throw's
    spawn offset travels it with `outerBox = false` (§5.7). Walls are visited in content order;
    a pass takes the nearest face, so order inside a pass never matters.
  - Statics live only in content: nothing is built at load or in `initialWorld`, nothing is in
    `WorldState`, snapshots and rollback are unaffected. Each move tests ≤ 32 boxes with a band
    rejection, cheaper than maintaining a grid. Measured with `kfm_replay bench --ticks 36000
    --players 10` (Release, GCC 15, the shared WSL2 development machine, `step()` alone, noisy
    within about ±15%): `dojo` 0.75–0.85 M ticks/s (1.2–1.3 µs/tick, the M1 figure: no walls
    takes the M1 path), `pillars` 0.74 M, `corridors` 0.85 M, `island` 0.83 M, and a synthetic
    worst case of 32 walls 0.37–0.47 M ticks/s (2.1–2.7 µs/tick). Even the worst case is about
    0.2% of a 60 Hz frame per simulated tick, so a rollback of 10 ticks stays far below a
    millisecond: no grid until an arena needs more than 32 statics.
  - Hazards: the pit is a rules-table row fed by a `Player × Hazard` contact (§5.3, §5.6; v0.57:
    the monkey falls for `fall_ticks` once its whole body is over the pits' union, a dash
    crosses it, and a pit narrower than a body takes nobody); a slow patch multiplies move
    speed in `ModifierSystem` (§5.8). Bananas ignore hazards (a
    grounded banana can lie in a pit; picking it up costs the monkey).
  - TOML (`[[arena]]`): `walls = [{ min = [x, y], max = [x, y] }, ...]`,
    `hazards = [{ kind = "pit" | "slow", min, max, speed_mul = m }, ...]` (`speed_mul` required
    for `slow`, in (0, 4], and an unknown key for `pit`). Validation: counts, `min < max` on both
    axes, boxes inside the arena, and no spawn point whose monkey overlaps a wall or whose centre
    is in a pit (touching a wall is fine). The arena files live in `content/arenas.toml` (`dojo`,
    index 0) and `content/arenas/NN-name.toml` (sorted after it; the numeric prefix fixes the
    order, append with a higher one).
  - Arenas (v0.22): `dojo` (index 0, the 32 × 18 box, unchanged), `pillars` (1: four 2 × 2
    blocks around an open centre), `corridors` (2: a 36 × 20 box split into four rooms by a cross
    of 0.6-thick walls with gaps at the centre and the ends, so throws into the next room come
    back around a wall), `island` (3: a 22 × 12 playfield in a pit that runs to the outer walls,
    a central block and two mud patches at half speed; bananas bounce off the outer walls back
    over the pit). Scripted 4-player matches (`kfm_replay stats`, 10 matches each) average
    rounds of 4.0 s on `dojo`, 5.0 s on `pillars`, 12.8 s on `corridors` (walls block sightlines:
    15% of throws hit against 37%) and 1.6 s on `island` (most eliminations are falls). One
    corpus replay per arena (§9).
- Content pipeline (v0.18, as built): TOML → `engine/content` → validated `ContentDB`;
  `content_hash` is the XXH32 of the canonical serialized form (the visitor), as before. Every
  number a system needs lives there, including the ones that are easy to forget: the pickup
  contact radius is `PickupDef::radius` (0.5), not the banana's flight radius (0.35), so a
  grounded banana is easier to walk over than to dodge.
- Files: `content/*.toml` (any depth), read in sorted relative-path order and merged. Top-level
  keys: `[player]`, `[boomerang]`, `[pickup]`, `[dash_attack]` exactly once in the whole set;
  `[[ability]]`, `[[arena]]`, `[[mode]]`, `[[powerup]]` (v0.20), `[[loadout]]` (v0.31) appended
  in load order. Keys are the C++ field names in
  snake_case; unknown keys and unknown top-level tables are errors (a typo never falls back to a
  default); keys other than `name`, `kind` and a few required lengths are optional and keep the
  struct default. Each file's header comment lists its keys and units.
- Values: integers are range-checked against the field's type; booleans are 0/1 switches;
  `Fixed` values are written as plain decimals (or integers) and converted exactly: the decimal
  is the rational n / 10^k and the Q16.16 value is n·65536 / 10^k truncated toward zero, the
  rounding of `Fixed::fromRatio` that built the M1 tables (so `0.35` is `fromRatio(35, 100)`),
  never through a `double`; at most 9 decimal places, no exponents. Speeds are written in units
  per second and divided by `kTickHz` in the same single truncation (`22` is
  `fromRatio(22, 60)`). Tags are lists of names (`dashing`, `perfect_dash`, ...), ability kinds
  are names (`dash`, `throw`); the loader's name tables static_assert they cover the enums.
- Names: abilities, arenas, modes, power-ups and loadouts have a `ContentName` (`std::array<char, 16>`, NUL padded,
  1–15 of `a-z 0-9 _ -`), hashed with the def. Defs reference each other by name in TOML (the
  player's loadout `abilities = ["dash", "throw"]`) and the loader resolves them to indices;
  ability index 0 is the built-in inert `none`. Arena, mode and loadout names are how command
  lines and the lobby choose (`ContentDB::findArena` / `findMode` / `findLoadout`); the sim only
  sees indices.
- Validation: duplicate names, unresolved references, counts against the capacities
  (`kMaxAbilityDefs` 32 since v0.70 (16 from v0.21, which the 15 shipped defs plus `none`
  filled; the cap sizes only `ContentDB::abilities`, so raising it moved the content hash and
  nothing else: def indices are a byte everywhere, in `AbilityState`, loadouts, grants and the
  bot observation, and the corpus checksums stayed byte-identical), `kMaxArenas` 16 since
  v0.33 (8 before), `kMaxModes` 32 since v0.31 (8 before),
  `kMaxPowerUps` 16, `kMaxLoadouts` 16 since v0.31; at least one arena and one mode), and
  the properties systems rely on instead of re-checking per tick (`validateContent`, also run
  on `defaultContent()` by a test): every loadout (the player's and each `[[loadout]]`) fills
  the dash and throw slots (v0.21; only the third may be `none`), loadout 0 is `base` with the
  `[player] abilities` (v0.31), a dash with no startup and its
  perfect window inside the active phase (v0.34: 0 allowed, and equal to it), arena half extents in (0, 1024], spawn points that
  keep a monkey inside (and, v0.22, clear of walls and out of pits), walls and hazards that are
  non-empty boxes inside the arena, a thrown banana clear of its thrower, `minAmmo >= 1`. Diagnostics are
  `file:line: error: message` lines; a failed load leaves the previous `ContentDB` untouched.
- Selection: a `MatchConfig` carries `arena` and `mode` indices (index 0 is the default, the
  first loaded), and since v0.31 a `loadout` index (0 = `base`). They are constant for a match like the content and are not part of
  `WorldState`, so world checksums and snapshots are unchanged; every host keeps the config next
  to the world. `step()`, every system, ability hook and rule handler takes a `MatchContent`
  (`db`, `arena`, `mode` references; `matchContent(db, config)`), and arena- or mode-dependent
  code reads `content.arena` / `content.mode`. A bare `ContentDB` converts to its default match,
  which keeps headless tests short. Indices are part of replays and the wire, and the arena and
  mode lists are part of the content hash: append new ones, never reorder.
- Where content comes from: every app resolves `--content DIR`, else `content/` next to the
  executable (CMake's `kfm_use_content()` mirrors the TOML there after each link), else the copy
  compiled into the binary (`EmbedContent.cmake` turns `content/**/*.toml` into byte arrays at
  build time). The browser client and every wasm test use the embedded copy (no file system
  needed); `kfm_replay` and `kfm_netsim` default to it too, so the determinism gate never
  depends on files beside the binary. Apps print the source directory and hash at startup;
  `kfm_replay info` shows the replay's and the loaded content's hash and the arena and mode.
- `defaultContent()` is the minimal headless test fixture: `constexpr` tables in `engine/sim`
  holding the base kit (the singletons `[player]`, `[boomerang]`, `[pickup]`, `[dash_attack]`,
  the abilities), the `dojo` arena and the `rounds` mode. It is what the sim unit and scenario
  tests run on (they must not depend on the loader) and the fallback of the client's sessions.
  The TOML under `content/` is a superset that Phase 2 extends as data only: new arenas, modes,
  power-ups and abilities go into TOML without being mirrored into `engine/sim/src/content.cpp`.
  A content test asserts that `content/` and the embedded copy agree with `defaultContent()`
  field by field (through the visitor) on the singletons, the abilities `defaultContent()`
  defines, arena 0 and mode 0, so the default match stays the fixture's; it does not compare the
  whole-DB hashes. A new field of an existing def struct still gets its default value in
  `defaultContent()`, because the struct changes anyway. The scenario tests' hand copies of
  shipped defs (`tests/scenario/loadout_util.hpp`: blink, slam, parry, `grid_call` and the
  ability loadouts; `powerup_util.hpp`: `throw_fast` and the five M4 power-ups) are checked
  against the loaded TOML the same way (v0.70, `tests/unit/content/mirror_test.cpp`), indices
  compared by the name they resolve to; it found the copy of `throw_fast` charging at the
  throw's rate rather than twice it (`charge_ticks` 20) and the power-ups without their
  visuals, both fixed in the copies.
- Adding content (any def, an arena or mode included) changes the content hash, which every
  replay carries, so it is followed by `regen-replay-corpus`; the existing `.checksums` must stay
  identical, because added content never changes the default match.
- Hot reload (native client, local play): with content from a directory and without
  `--no-hot-reload`, the client polls the directory's `*.toml` modification times and sizes
  about once a second; on a change it reloads, prints diagnostics and keeps the old content on
  error, else replaces the loaded content (dropping debug-UI tuning edits), keeps the arena and
  mode when they still exist and restarts the match with the same seed (or re-creates the lobby
  world). Online the server's content and config rule, so it is off; the browser has no
  directory to watch.
- Staging: in M1 the defs (Dash, Throw, banana, one box arena, FFA rules) were `constexpr`
  tables only; M4 put the TOML loader in front without touching how systems read defs.
- Modes (v0.23, `content/modes.toml`, `ModeDef` in `kfm/sim/content/mode.hpp`): `kind` =
  `rounds` (the default when the key is absent), `deathmatch` or `survival`; shared
  `round_start_ticks`, `round_end_ticks` (read by the round kinds only; 180 since v0.55);
  `rounds_to_win` (rounds, survival);
  `time_limit_ticks` (deathmatch: the match, survival: each round; 0 = no clock); `score_to_win`,
  `respawn_ticks`, `respawn_invuln_ticks` (deathmatch). A kind ignores the numbers it does not
  read (they keep their struct defaults and are hashed like every field). Validation: a known
  kind, `rounds_to_win >= 1`, and in deathmatch `score_to_win >= 1` and `respawn_ticks >= 1`.
  The file ships `rounds` (index 0, the M1 numbers), `deathmatch` (first to 10, 3 min, 2 s
  respawn, 1.5 s protection) and `survival` (first to 3 rounds of at most 60 s). From v0.21 to
  v0.30 any kind could carry `abilities = [...]`, its own loadout (`ModeDef::hasAbilities`/
  `abilities`), and the file shipped `rounds_blink`, `rounds_slam` and `rounds_parry`; since
  v0.31 the kit is its own axis (Loadouts below) and the key is an error naming
  `content/loadouts.toml`. The modes stay in one file so their order, and so their indices, is
  explicit.
- `DashAttackDef` (v0.16): `damage` (1), `minAmmo` (1: a banana in hand), `consumesAmmo`
  (0: kept). Visited for the content hash like every other def; the TOML key is
  `[dash_attack]`. The loader's validation requires `minAmmo >= 1` like the M1 tables did;
  allowing 0 (every dash an attack) is a one-line change there when a mode wants it.
- Power-ups (v0.20, §5.8): `[[powerup]]` with `name`, `duration_ticks` (required, >= 1),
  `color` (palette 0–9), `shape` (`orb` | `cube` | `diamond` | `coin`) and `grants` (required,
  1–4 inline tables): `{ kind = "stat", stat = "move_speed" | "max_hp" | "ammo_max", mul, add }`,
  `{ kind = "ability", slot = "dash" | "throw", ability = "<name>" }`,
  `{ kind = "projectile", speed_mul, radius_mul, damage_add }`,
  `{ kind = "tag", tag = "shielded" | "invulnerable" }`. An arena's optional `spawners` is a list
  of up to 8 `{ pos = [x, y], power_ups = ["<name>", ...], first_ticks, interval_ticks,
  max_alive }` (`power_ups` absent = every power-up). Ability and power-up names resolve after
  every file is read, so the reference may point into a later file. Validation: unknown names,
  an unknown grant kind or stat, a `mul` or a fractional `add` on an integer stat, an override of
  another kind than its slot's def, a tag a power-up may not grant, multipliers outside (0, 4],
  a spawner outside the arena (by the pickup radius), inside a wall or in a pit (v0.22 statics), spawners without any power-up defined, an
  arena's `max_alive` sum above 16.

- Modes, second cut (v0.29, §5.3): `ModeDef` gains `space` (`continuous` | `grid`), `turnTicks`
  (grid, 1–60), `teams` (0 = free-for-all, else the team count the lobby distributes the slots
  over), `baseline` (a power-up name resolved to an index, none by default), `roles` (up to 10
  loadout names assigned by position within a team, round-robin; team modes only), `hillTicks`
  (hill), the `vision` block §8 reads (`vision = { kind, peripheral, cone, focus, hearing,
  banana_sight }`) and the kinds `team_rounds`, `team_deathmatch`, `hill`, `capture`. `kMaxModes`
  rises to 32; every slot is hashed, so the content hash changes once (corpus regenerated,
  checksums unchanged). As built (v0.31): `kMaxModes` is 32; the other fields arrive with the
  later steps of §11. As built (v0.32): `teams` (u8; validation: 0 or 2..10, and the team kinds
  need 2 or more), `baseline` (u8 power-up index, `kNoBaseline` = 0xFF; the TOML name resolves
  after every file is read, an unknown one is an error) and `roles` (`roles[10]` loadout
  indices plus `roleCount`; names resolve the same way; only with `teams > 0`), visited after
  the deathmatch numbers. Shipped: `team_rounds` (index 3: 2 teams, first team to 3 round wins)
  and `team_deathmatch` (index 4: 2 teams, first team to 20, 3 min, deathmatch's respawn
  numbers), neither with a baseline or roles.
  As built (v0.33): `hillTicks` (u16, TOML `hill_ticks`, >= 1, default 300) and
  `flagReturnTicks` (u16, `flag_return_ticks`, >= 1, default 600), visited after `roleCount`.
  `hill` and `capture` are team kinds (`teams` >= 2) and validate like deathmatch
  (`score_to_win`, `respawn_ticks` >= 1, the message naming the kind). Shipped: `hill` (index
  5: 2 teams, a point per 5 s alone, first to 5, 3 min, deathmatch's respawn numbers) and
  `capture` (index 6: 2 teams, first to 3 captures, a dropped flag home after 10 s, 3 min, the
  same respawns).
- Loadouts (v0.29) are their own axis. `[[loadout]]` = `name` plus 2–3 ability names
  (`ContentDB::loadouts`, `kMaxLoadouts` 16; index 0 is `[player] abilities`).
  `MatchConfig::loadout` picks one for the match (the lobby, `--loadout NAME`, `?loadout=`) and
  rides in `MatchContent`, the replay header (format version 4) and the join messages (protocol
  version 4). `ModeDef::abilities` and `hasAbilities` go away: `rounds_blink`, `rounds_slam` and
  `rounds_parry` leave `content/modes.toml` for `content/loadouts.toml` (`blink`, `slam`,
  `parry`), and `MatchContent::loadout()` returns the role's loadout in a team mode, else the
  match's. Corpus replays that named those modes are re-recorded with `--loadout`.
  As built (v0.31, `kfm/sim/content/loadout.hpp`): `LoadoutDef{name, abilities[3]}`; the
  loader puts loadout 0 in place before any file is read, named `base` (reserved: a
  `[[loadout]]` of that name is an error) with the resolved `[player] abilities`, and appends
  each `[[loadout]]` (`name`, required `abilities`; unknown keys are errors) after it, at most
  15. `ContentDB::loadoutAbilities(i)` is the kit per slot, and for index 0 (or an index out of
  range) it reads `PlayerDef::abilities` itself rather than the mirror in `loadouts[0]`, so code
  and tests that edit the player's kit play what they say; `validateContent` holds the mirror
  equal. `MatchContent::loadoutIndex` is the selection and `loadout()` returns
  `loadoutAbilities(loadoutIndex)`, read by `resetPlayer` at every spawn and by
  `ModifierSystem` when an override ends; there is no role lookup until teams (step 2).
  As built (v0.32): `MatchContent::loadoutIndexFor(slot)` is the role in a team mode with
  roles (the slot's position among the slots of its team in slot order, round-robin over
  `roles[0..roleCount)`), else `loadoutIndex`; `loadout(slot)` its abilities, read by
  `resetPlayer` and `ModifierSystem`. `loadout()` without a slot stays the match's.
  `defaultContent()` holds loadout 0 only, and the content test compares loadout 0 field by
  field like arena 0 and mode 0. `content/loadouts.toml` ships `blink`, `slam`, `parry` (the base
  kit plus the third ability). The lobby panel gains a Loadout row after Mode (the preview
  lists its abilities), the settings file a `loadout=` line (a stale name, `rounds_slam` from
  a v0.24 file included, warns and is skipped), and every host takes `--loadout NAME`
  (`content::resolveLoadoutName`): `kfm_client`, `kfm_server`, `kfm_netsim`,
  `kfm_render_probe`, `kfm_replay record-*`/`bench`/`stats`, `kfm_arena`; the browser's
  `?loadout=` maps onto the flag. Since loadouts are a `MatchConfig` axis like the power-up set,
  no world checksum changed: the three ability replays were re-recorded with `rounds` plus their
  loadout and kept their `.checksums` byte for byte.
- Teams (v0.29): `MatchConfig::teams`, a team index per slot (free-for-all: slot i is team i);
  the lobby fills it from `ModeDef::teams`, balanced, joiners to the smallest team. Arenas gain
  `spawn_groups` (default 1): the spawn list splits into that many consecutive groups and team
  t spawns from group t mod groups; validation requires every group to hold a point.
  As built (v0.32): `ArenaDef::spawnGroups` (u8, default 1, visited last), TOML `spawn_groups`
  in [1, 10] and validated against the point count; group g holds points [g·n/G, (g+1)·n/G)
  (`spawnGroupRange`). `ContentDB::compatible(arena, mode)` is `mode.teams == 0 ||
  spawnGroups >= mode.teams`, with `firstCompatibleArena(mode)`; the sim itself plays any pair.
  `content::resolveMatchNames` (every command line, the client's flags and `?mode=`) takes the
  first compatible arena when no arena is named and refuses a named incompatible one with the
  list of arenas that suit; the client's lobby settings let a saved arena give way instead; the
  lobby's Arena row skips incompatible arenas and a mode change moves the arena to the first
  compatible one. `dojo` keeps one group: in a free-for-all team t is slot t, so two groups would
  move slots 1 and 3 to the other half and change every dojo checksum. The team arena is new,
  `content/arenas/05-standoff.toml` (`standoff`, index 5): the dojo's 32 × 18 box, two 2 × 3
  blocks in the middle with an open lane between them, and two groups of five points at
  x = ∓11..13.5 facing each other.
- Zones (v0.29): `zones = [{ min, max }, ...]` (`kMaxArenaZones` 8), boxes like hazards that
  block nothing and emit nothing; `zoneOccupants(s, arena, k)` returns the mask of the monkeys
  whose centre is inside, for `onPlaying`. `hill` needs one zone, `capture` one per team.
  As built (v0.33): `ArenaDef::zones` (`Zone{min, max}`, `Zone::centre()`) and `zoneCount`,
  visited after `spawnGroups` with `cellSize`. TOML `zones = [{ min = [x, y], max = [x, y] },
  ...]` (unknown keys are errors); validation as the statics' boxes: non-empty and inside the
  arena, at most 8. Nothing in `CollisionSystem`, `moveCircle` or the contacts reads a zone.
- Tilemap arenas (v0.29): an `[[arena]]` may give `cell = 1.0` and `map = ["...", ...]` in place
  of `half_extents`, `walls`, `hazards`, `spawn_points` and `zones`: rows of equal length, one
  glyph per cell, `#` wall, `.` floor, `~` pit, `:` mud (`mud_speed_mul`, default 0.5), `0`–`9`
  spawn points in that order, `a`–`h` zone cells. The loader merges runs of a glyph into
  axis-aligned boxes (along a row, then equal rows vertically) and fails over the statics caps
  rather than raising them; `halfExtents` is the map size times the cell. A tilemap arena
  records `cellSize` (0 for a hand-placed arena) and plays in either space; a grid mode requires
  `cellSize > 0`. `ContentDB::compatible(arena, mode)` collects these requirements (cells,
  zones, spawn groups at least the team count) for the lobby and the command lines; an
  incompatible pair is a configuration error, never a load error.
  As built (v0.33, `engine/content/src/loader.cpp` `parseTilemap`): an `[[arena]]` with `map`
  takes `cell` (required, 0.25 to 16) and `mud_speed_mul` (default 0.5, (0, 4]); giving
  `half_extents`, `walls`, `hazards`, `spawn_points`, `spawn_ellipse` or `zones` with it is an
  error, and so are `cell`/`mud_speed_mul` without it; `spawn_groups` and `spawners` work as
  in a hand-placed arena. Glyphs:

  | Glyph | Cell |
  |---|---|
  | `#` | wall |
  | `.` | floor |
  | `~` | pit (`HazardKind::Pit`) |
  | `:` | mud (`HazardKind::Slow`, `speedMul = mud_speed_mul`) |
  | `0`–`9` | floor with a spawn point at the cell's centre; digit order is spawn order; each digit once, no gaps |
  | `a`–`h` | floor in zone `letter − 'a'`; each letter's cells must fill one rectangle, letters without gaps |

  At most 256 rows of at most 256 cells, all of one length; the top row is +y, the origin the
  map's centre, `halfExtents = (columns, rows) × cell / 2`, which must be exact in Q16.16 and at
  most 1024. Merge (per glyph class): each row's maximal runs, then a run whose columns equal
  those of a box ending on the row above extends that box downward (an unequal run starts a new
  box); boxes come out by top row, then left column. Walls are the `#` boxes; hazards are the pit
  boxes, then the mud boxes. A map that merges into more than `kMaxArenaWalls` (32) walls or
  `kMaxArenaHazards` (16) hazards fails with the count ("simplify the walls"); the caps are
  never raised. `cellSize` is the cell (0 for a hand-placed arena). The result then runs the
  hand-placed validation (spawn points clear of walls by the player radius, out of pits, inside
  the box), so a spawn cell beside a wall needs a cell of at least twice the radius. Tilemaps
  play in the continuous space today; step 4's grid reads `cellSize`. `compatible(arena, mode)`
  as built: spawn groups at least `mode.teams` (v0.32), `hill` at least one zone, `capture` at
  least `mode.teams` zones; the grid's `cellSize > 0` joins with the `space` field (step 4).
  The pair `outposts`/`hill` is compatible (zone 0 is the west base); the lobby's first pick for
  `hill` is `hilltop`, and judging fairness of a zone is left to content. `kMaxArenas` rose to
  16 (v0.33): `hilltop` and `outposts` filled the 8, and the loader test appends one more; the
  arena is a u8 in replays and the join messages, so only the content hash changed.
  Shipped: `content/arenas/06-hilltop.toml` (`hilltop`, index 6, the dojo's 32 × 18 at one
  cell per unit: a 4 × 4 hill at the centre between two low walls, four flank blocks, two
  centre walls, four mud patches, two groups of five, symmetric under a half turn; 8 walls, 4
  hazards) and `07-outposts.toml` (`outposts`, index 7: a 4 × 4 base at either end, zone 0
  west and 1 east, each group beside its base, two pits on the centre line at the edges, mud in
  the corners; 8 walls, 6 hazards).
- Contact radius as a stat (v0.29): `Stats::radius` (base `PlayerDef::radius`) replaces the
  direct reads in `CollisionSystem`, `MovementSystem` and the spawn checks, and `StatModifier`
  gains `stat = "radius"`. Without a modifier the value is the def's, bit for bit. Grid content
  shrinks monkeys to 0.35 and bananas (through the existing projectile `radius_mul`) to 0.15, so
  the hit disc of 0.5 sits inside a one-unit cell: "a banana whose centre crosses my cell hits
  me" is exact except at the corners.
- Rooting (v0.29): `AbilityDef::rootPhases`, a phase mask during which movement intent is
  ignored (continuous) or no new cell commitment is taken (grid); the Blink/Slam startup
  follow-up uses the same field. The grid throw roots through its recovery, which the content
  sets to `turnTicks`, so a turn is move or throw; the `quick_hands` power-up overrides the
  throw with an unrooted def, which is the only way to move and throw in one turn.
- As built (v0.34, M5b step 4). Modes: `ModeDef::space` (`Space::Continuous` | `Grid`, TOML
  `space = "continuous" | "grid"`, default continuous), `turnTicks` (u8, `turn_ticks`, 1–60,
  default 15) and `dashCells` (u8, `dash_cells`, 1–8, default 2): the dash's distance lives on
  the mode next to the turn, since both are the space's numbers and every grid kit dashes the
  same distance; `turn_ticks` and `dash_cells` in a continuous mode are errors ("belongs to a grid
  mode"). `ModeDef::arena` (u8, TOML `arena = "<name>"`, `kNoArena` 0xFF by default) names the
  arena a host picks for the mode when none is named: `firstCompatibleArena(mode)` returns it
  when it suits, else the first compatible arena as before; validation requires it to exist and
  suit the mode. Added because the first arena with cells is `hilltop`, while the grid modes
  want their own smaller arena; the lobby's Mode row brings a mode's own arena with it. All four
  are visited after `flagReturnTicks` (the content hash changed, checksums did not).
  `compatible(arena, mode)` adds `mode.space == Grid` ⇒ `cellSize > 0`; the "does not suit"
  message of `resolveMatchNames` names all three requirements.
  Contact radius: `Stats::radius` is derived by `ModifierSystem` like `moveSpeed` (§5.8) and set
  at every spawn; read by `CollisionSystem` (body radius and both pair radii), both spaces'
  moves, the blink's jump, and Observation v1's `self.radius` (`rules.playerRadius` stays the
  def's; the reference bot takes its own radius for a dashing enemy's, since a baseline changes
  every monkey's alike). Nothing else read the def's radius in the sim: the spawn checks are the
  loader's (spawn points against `PlayerDef::radius`, the largest radius content gives a
  monkey in the shipped modes), and the respawn, round start, mode hooks, slam reach and the
  pickup and dash-attack rows read positions and the collision's contacts only. The field
  entered the world state with the commitment (one corpus regeneration); switching the reads
  to it changed no checksum.
  Rooting: `AbilityDef::rootPhases` is a mask of `phaseBit(phase)` (TOML `root_phases`, a list of
  `startup`, `active`, `recovery`; visited after `tagsRecovery`); `rooted(p, content)` is true
  while any slot runs a phase its def roots. The Blink/Slam startup follow-up stays open: the
  shipped continuous defs root nothing (checksums unchanged).
  Grid content (`content/abilities.toml`, `loadouts.toml`, `powerups.toml`, `modes.toml`,
  `arenas/08-courtyard.toml`), §5.12's starting numbers at 15-tick turns:

  | Number | Design | Content | Arithmetic |
  |---|---|---|---|
  | cell | 1.0 | `courtyard` `cell = 1.0` | |
  | monkey radius | 0.35 | `grid_base`: `stat = "radius", mul = 0.7` | 0.5 × 0.7: 32768 × 45875 >> 16 = 22937 raw = 0.35 truncated |
  | banana radius | 0.15 | `radius_mul = 0.4286` | 0.35 × 0.4286: 22937 × 28088 >> 16 = 9830 raw = 0.15 truncated; hit disc 22937 + 9830 = 0.49998 |
  | HP | 2 | `stat = "max_hp", add = 1` | |
  | banana speed | 2 cells per turn | `speed_mul = 0.5` | 16 u/s × 0.5 = 8 u/s = 8 / 60 × 15 = 2 cells per 15 ticks (17476 raw/tick × 0.5 = 8738; × 15 = 1.99997) |
  | throw range | 6 cells | not reachable: 4.8 cells out | `outbound_ticks` is the `BoomerangDef`'s (36): 8 u/s × 0.6 s = 4.8 cells (2.4 turns), then it turns home; 6 cells would need 45 ticks, and no power-up reaches the def's ticks |
  | walk | 1 cell per turn | the slide takes `turn_ticks` (15) | 4 steps a second |
  | dash | 2 cells | `dash_cells = 2`, `grid_dash` `active_ticks = 15` | 2 cells over one turn: twice the walk, `Dashing` throughout |
  | dash cooldown | (base 0.75 s) | `grid_dash` 45 ticks | 3 turns |
  | catcher cooldown | 4 turns | `catcher_dash` 60 ticks, window 15 | 4 × 15; the window spans the active phase |
  | throw | move or throw | `grid_throw` startup 2, active 1, recovery 15 rooted, cooldown 15 | the recovery holds exactly one latch whatever the press tick, since the latch reads the state of the tick before |

  Kits: loadouts `grid` (`grid_dash`, `grid_throw`) and `catcher` (`catcher_dash`, `grid_throw`);
  `grid_rounds` (index 7: team rounds, two teams, first to 3, `space = "grid"`, `turn_ticks =
  15`, `roles = ["catcher", "grid", "grid", "grid", "grid"]`, so each team's first slot is its
  catcher, `baseline = "grid_base"`, `arena = "courtyard"`). The bot-speed variant is
  `grid_rounds_1t` (index 8; the task's `grid_rounds_fast` does not fit a 15-character
  `ContentName`): `turn_ticks = 1` with the `_1t` kits (`grid_dash_1t`: 1 active tick, a 3-tick
  cooldown, window 0; `catcher_dash_1t`: window 1 of 1, a 4-tick cooldown; `grid_throw_1t`: no
  startup, recovery 1 rooted) and `grid_base_1t`, whose bananas fly at the projectile
  multiplier's limit of 4 (64 u/s, about 1.07 cells per turn) where 2 cells per turn would need
  7.5 (open, §12). `quick_hands` (power-up index 5, 600 ticks) overrides the throw slot with
  `quick_throw` (no startup, no recovery, no root, cooldown 1), shared by both turn lengths;
  the courtyard's two spawners drop it. The baselines are power-ups 6 and 7 with
  `duration_ticks = 1` (required, never counted for a baseline); no shipped spawner draws them:
  `dojo_powerups`' four spawners, which named no set and so drew from every power-up, now name
  the five they always drew from (the same draws, checksums unchanged). The lobby lists no
  toggle for a power-up some mode names as its baseline. The arena is `courtyard` (index 8): a
  24 × 14 tilemap, two groups of five spawn points on the west and east edges, two 2 × 2 blocks
  and a 1 × 4 pillar per side, a 2 × 2 block in the middle, two pits on the centre line at the
  edges (v0.57: 2 × 2, so the continuous modes' whole-body rule can take a monkey too; they were
  2 × 1), symmetric under a half turn (7 walls, 2 hazards); `hilltop` and `outposts` suit the
  grid modes too.
- Vision (v0.35, `ModeDef::vision`, `VisionDef` in `kfm/sim/content/mode.hpp`): `vision = {
  kind = "all" | "team" | "own", peripheral, cone, focus, hearing, banana_sight }`, an inline
  table read only by the observation builder (§8), never by the sim; visited after `arena`, so
  the content hash changed once (every `.checksums` byte-identical). `kind` is required in the
  table, the default is `all` with zeros; the radii are cells (u8): `cone` and `hearing` at
  most 32, `peripheral`, `focus` and `banana_sight` at most 16 (`kMaxVisionCone`,
  `kMaxVisionHearing`, `kMaxVisionRadius`); an `all` vision with a radius is an error (it sees
  everything anyway), `team` needs `teams >= 2`, unknown keys are errors. The radii being
  cells, `ContentDB::compatible` refuses a fog kind (`team`, `own`) on an arena without cells,
  as it refuses a grid mode; a continuous mode on a tilemap may use fog (the cone then reads
  the facing every tick). Shipped: `grid_rounds` and `grid_rounds_1t` both `{ kind = "own",
  peripheral = 1, cone = 4, focus = 1, hearing = 3, banana_sight = 1 }`, §8's starting numbers:
  the cone reaches two turns of 15-tick banana flight (2 cells a turn) and two thirds of the
  6-cell throw design (4.8 cells as shipped), hearing is one turn of warning plus a cell; in the
  1-tick variant the bananas fly about 1.07 cells a turn, so the same numbers give hearing three
  turns of warning (§12). The four grid kits gain the call on the ability button (`grid`,
  `catcher`: `grid_call`; `grid_1t`, `catcher_1t`: `grid_call_1t`), which changed the two grid
  corpus entries' checksums once (each monkey's third `AbilityState` now names a def, and the
  scripted generator presses it); a 5v5 entry, `scripted_10p_grid_rounds_1t_1800t` on
  `courtyard`, is new.

### 5.11 Timing precision: 60 Hz plus sub-tick input timestamps

The dash-catch lives or dies on timing consistency, so the tick quantum matters. Three options:

| Option | Resolution | Cost |
|---|---|---|
| 60 Hz tick | 16.7 ms for everything | baseline |
| 120 Hz tick | 8.3 ms for everything | 2× sim and rollback work; only improves input timing if the client really samples input and steps at 120 Hz in real time (a 60 fps render loop running two steps per frame samples input once, so both steps see the same input) |
| 60 Hz + sub-tick timestamps | ~65 µs for *when a press happened*; 16.7 ms for *when state changes* | one byte per input; a few rules become time-aware |

We take the third. Each `PlayerInput` carries `subTick`: the fraction of the current tick at which
the most recent press edge occurred (§5.4), derived from the OS event timestamp (SDL3 timestamps events
in nanoseconds) relative to the tick start. It travels in the input stream, so it is deterministic,
replayable, and rolls back like everything else.

What uses it:

- **Perfect-window judgment.** `CollisionSystem` runs swept circle tests (needed anyway to stop
  fast bananas tunnelling through monkeys), so every contact has a time of impact `tHit` in
  fractional ticks. The catch rule compares `tHit` with the press time `T + subTick/256` rather
  than with the tick boundary: perfect if `0 ≤ tHit − pressTime < perfectWindow`. The window
  stays 3–5 ticks wide, since humans do not reliably hit tighter than one 60 Hz frame and
  fighting games treat a frame as the finest meaningful window. The stamp removes the ±1 tick
  jitter at the window edges, which is what makes timing feel inconsistent. The dash itself still
  starts at the tick boundary; only the judgment uses the stamp.
- **Deterministic tie-breaking.** Two monkeys dashing for the same banana in the same tick:
  `InteractionSystem` resolves contacts in `tHit` order, so the monkey the banana reaches first
  wins; on equal `tHit` the earlier dash press wins, compared as (`activatedTick`,
  `activatedSubTick`) of player A when it holds the tag its rows key on (`PerfectDash` against a
  banana or pickup, `Dashing` against a player, v0.16), and a player without it sorts after
  every dashing one; then the lower pool index (`kindA, indexA, kindB, indexB`), which for
  players is the slot (§5.6, v0.14). Two players dashing into each other is the same tie: the
  earlier press strikes first (§5.6).
- **Telemetry.** The sim knows the true press time, so input-to-effect latency can be measured
  in real matches.

What stays tick-quantised: movement integration, ability phase timers, spawns and despawns,
modifiers. Positions change only at tick boundaries. Full CS2-style sub-tick, where entities move
piecewise within a tick and every event is ordered in continuous time, would push fractional time
through every system. It is not worth it for a game at this speed, and the targeted version can
grow one rule at a time without touching the protocol.

Escape hatch: inputs and packets are decoupled in the input-sync model, so a 120 Hz sim with
60 packets/s (two inputs per packet) is possible later. It needs a real-time 120 Hz client step
loop (a sim thread or a 120 fps render loop) to gain anything for input timing. Treat it as a
measured experiment after M2 playtesting, not a default.

### 5.12 Spaces: continuous and grid (v0.29)

A mode's `space` chooses how monkeys move, and nothing else. `continuous` is the sim as built,
bit for bit. `grid` turns the arena into cells for the monkeys and leaves everything else where
it is, which is the whole design: flight, collision, the rules table, snapshots, rollback, the
renderer and the bot ABI see Q16.16 positions either way, and a grid monkey is a circle whose
centre happens to rest on a cell centre.

- **Behaviour table.** `SpaceBehavior` (`kfm/sim/spaces/registry.hpp`, one file per space under
  `src/spaces/`, constexpr like the ability and mode tables) with two hooks: `latch(s, content,
  frame)` called by `InputSystem` and `move(s, content)` called by `MovementSystem`. The
  continuous behaviour's `latch` is a no-op and its `move` is today's `movementSystem` body.
- **Turns.** `turnTicks` ticks (content, 1–60). A turn starts on every Playing tick with
  `ticksInPhase % turnTicks == 0`; phases, round timers and the mode clock run in ticks exactly
  as before, so `RulesSystem` knows nothing about turns. Bot-only matches set 1 and run at full
  speed in `kfm_arena`; human lobbies set about 15 (four moves a second).
- **Commitment.** At the latch each monkey that is alive, not rooted and at rest takes the
  latched intent: idle, or one of the eight neighbouring cells. The target must lie inside the
  outer box and overlap no wall (a pit is allowed: the monkey falls, §5.6 row 60; v0.57: once
  its disc is inside the pit cell, as in the continuous space, near the end of the slide).
  Contested
  cells resolve at the latch: two monkeys that target one cell, or each other's cell, keep the
  earlier press (`subTick` of the latch tick), then the lower slot; the loser stays. Nothing
  about this is a rule row: the table says what happens when things touch, the space says who
  gets to move. The commitment is state, `Player::moveTarget` (Vec2) and `Player::moveTicks`
  (0 = at rest), visited like every field; the continuous space leaves them zero, so its
  checksums change once when the fields land (corpus regenerated).
- **Slide.** A committed monkey moves `(target − pos) / moveTicks` per tick through
  `statics::moveCircle` like any other monkey and snaps to the target exactly on its last tick,
  so rounding never accumulates. Mid-turn it is genuinely between cells and only the continuous
  contact test has an answer; at the turn boundary, where bots observe and act, every monkey is
  on a cell. A dash is a commitment of `dashCells` (content, 2) with `Tag::Dashing` for its
  active phase; a wall stops it on the last free cell. Knockback, hit stun and pits act on the
  position as in the continuous space (v0.57: the slide is `Dashing` throughout, so a 2-cell
  dash crosses a one-cell pit and falls only if it ends on a pit cell; a falling monkey keeps
  no commitment and takes no latch).
- **Facing.** `Transform::facing` is the latched `aimDir`. The cone (§8) quantises it to the
  nearest of eight directions when it reads.
- **Bananas.** Untouched: thrown at any of the 256 angles, flying, steering home, bouncing off
  walls and swept against monkey circles by `ProjectileSystem` and `CollisionSystem` exactly as
  in §5.7. A banana has no cell in the sim; the observation builder reports the cell of its
  centre next to its exact position (§8).
- **Move or throw.** The grid throw roots through its recovery phase, `turnTicks` long (§5.10),
  so a monkey that throws takes no commitment at the next latch; a slide in progress finishes.
  Rotation is aim and never costs anything.
- **The catch.** A bot has perfect timing, so a reflex catch is free for it and the loop breaks:
  every seen banana would be caught, throws would work only from blind spots, and hearing (§8)
  would close even those. On the grid the base dash has a perfect window of 0 ticks, which
  validation now allows: `PerfectDash` is never granted and row 10 never matches. One loadout,
  `catcher`, carries a dash whose window spans the active phase with a cooldown of several
  turns, and `roles` give it to one monkey per team. Scarcity, not timing, is the balance: the
  cooldown turns the catch from a reflex into a resource, and killing the catcher disarms a
  team's defence. Second levers if it is still too strong: the catch only inside the facing cone,
  and HP 2 so one caught banana does not swing the fight.
- **Numbers to start with** (grid content, tuned with `kfm_arena`, open in §12): cell 1.0,
  monkey radius 0.35, banana radius 0.15, HP 2, banana speed 2 cells per turn, throw range 6
  cells, dash 2 cells, catcher cooldown 4 turns.
- **Presentation.** Nothing new: the interpolated slide reads as a hop; cones and fog are not
  drawn for humans (§8).

As built (v0.34, step 4; `src/spaces/grid.cpp`, `kfm/sim/spaces/grid_cells.hpp`). Where it
differs from or adds to the design above:

- **Cells.** One mapping, shared with step 5's observation builder (`engine/ai` may include
  it): column 0 is the arena's west edge, row 0 its north (+y) edge, so `(col, row)` reads like
  the tilemap; `cellOf` floors `(x + hx) / cell` and `(hy − y) / cell`, so a point on a vertical
  edge belongs to the cell east of it and one on a horizontal edge to the cell south of it;
  `cellCentre` is exactly where the tilemap loader puts a spawn point (a test checks every
  shipped tilemap's points), so a grid monkey spawns at rest on a centre. An arena without
  cells maps everything to cell 0 and has no columns; the sim plays an incompatible pair with
  nobody moving.
- **The latch, precisely.** A step needs the target cell in the arena's columns and rows, the
  monkey's circle (its radius stat) at the target's centre inside the outer box and clear of
  every wall, and for a diagonal the two orthogonal neighbours clear as well: no corner
  cutting, so the slide's straight path never meets a wall and lands exactly. A pit is
  allowed. Eligible: alive, not `Eliminated` (v0.57: nor `Falling`), not in `HitStun`, not rooted, at rest. A monkey
  that stays (idle, blocked, rooted out, or the loser of a contest) and stands off its cell's
  centre (a slam's push, a stun that cut a slide, a blink) commits to that centre, so the turn
  boundary puts it back on a cell.
- **Contested cells** are judged in one pass over the steps taken at the latch: a step loses to
  another that targets the same cell or swaps with it and has the lower key (`subTick` of the
  latch tick, then slot). No cascade: a winner keeps its target even when the loser of a swap
  stays in it (they then share the cell), and a step that loses one contest still blocks the
  steps it beat. Moving into a cell another monkey stands in is no contest: monkeys overlap in
  the continuous space too, and the dash attack needs exactly that. A dash bypasses the
  resolution (it is an ability, committed in the move).
- **The slide.** A walk is `moveTicks = turnTicks`, so it lands on the next latch. Each tick the
  velocity is `(moveTarget − pos) / moveTicks` (Fixed division truncates; the last tick's step is
  the exact remainder) through `moveCircle`, so an unobstructed slide ends on the target bit for
  bit, which the scenario suite checks over seventeen turns. A slam's knockback adds on top and can
  deflect it; a slide that ends off target simply ends (the next latch re-centres). `HitStun`
  ends a commitment where the monkey stands (in the continuous space it zeroes the velocity).
  Slow hazards and `move_speed` have no say on the grid.
- **The dash timing decision.** The dash slides over its def's `activeTicks`: the grid content
  sets them to one turn (15, or 1), so the dash is a turn's commitment of two cells and
  `Dashing` (with the catcher's window) lasts exactly as long as it moves. It is committed in the
  move on the dash slot's first Active tick (phaseTicks 0), from the cell holding the monkey's
  centre (mid-slide: the cell it has reached) along the octant of the dash's direction, each step
  checked like a walk, so a wall stops it on the last free cell. It replaces a walk in progress,
  including the one its own latch tick committed. A rooted monkey can still dash (as in the
  continuous space, where the dash's velocity always overrode the stick). The dash def's `speed`
  is never read on the grid.
- **Move or throw.** As designed: the latch at tick L reads the phases as the tick before left
  them, so with recovery = `turnTicks` exactly one latch is rooted for any press tick; a throw on
  a latch tick goes with that latch's step (the slide in progress finishes) and costs the next
  one.
- **Facing** is set at every latch during Playing (rooted or not) and follows aim every tick
  outside Playing; the continuous latch is `facing = aim` every tick (the doc's "no-op" holds the
  line `InputSystem` had).

What the grid is not: it is not a second sim. One monkey per slot stays load-bearing
(`Pool<Player, kMaxPlayers>` indexed by `PlayerId`, the roster mask, the observation), so a bot
"with an army" is a team of slots (§8) and `kMaxPlayers` caps a side at five. Armies of dozens
would be a different `WorldState` and a different game; if that day comes it is a sibling sim
over `core`, not a switch in this one.

Alternatives considered (v0.29), so they are not re-proposed without new evidence:
- Facing from the last move, with rotation costing a turn: rejected. It makes retreating a blind
  walk, so the dominant play against an unseen threat is to stand still, and it kills kiting,
  the throw-dodge-catch loop the game runs on. Free rotation makes facing a bet about where the
  threat is, which fog keeps interesting.
- A cell-coincidence narrowphase (a banana hits when its centre's swept segment crosses the
  monkey's cell): the deliberate fallback, not the first cut. Radii tuned so the hit disc sits
  inside the cell need no code; the narrowphase is one variant behind the space table and the
  same `Contact`, to be built only if bot authors trip on the corner cases.
- Bananas on eight-direction cell paths: rejected in favour of continuous flight. Paths need a
  new projectile system and lose curved returns and bank shots; continuous bananas reuse
  everything and give any throw angle for free.
- One program per team (`kfm_act(tick, slot)`) and a shared team array first: rejected in §8.
- A second, turn-based sim: rejected above; the only line that would reopen it is more than one
  monkey per slot.

---

## 6. Networking (`engine/net`)

### 6.1 Model

**Server-authoritative, deterministic, input-synchronised, client-side rollback.**

- Server runs `sim` at 60 Hz. For tick T it assembles `InputFrame(T)` from inputs received
  for T, substituting "repeat last input" for any player whose input has not arrived, steps,
  and broadcasts `InputFrame(T)` as authoritative. **The server never waits for a slow client.**
- Client runs ahead of the server by a controlled lead. Each local tick L it samples input,
  sends it (with redundancy), applies it immediately as a prediction, predicts remote players
  with "repeat last known input", saves the state, and steps. When authoritative
  `InputFrame(T)` arrives and differs from the prediction for T, it restores the saved state
  for T and re-simulates T..L with corrected inputs. The ring buffer holds `kRollbackWindow`
  states (e.g. 32 ticks ≈ 530 ms).
- Full-state snapshots are used only for join, spectate, reconnect and checksum-mismatch
  resync, never in the steady state.

Why not the alternatives:

- *Snapshot interpolation (Quake/Source style)*: remote entities render one snapshot interval
  plus a buffer behind (~100 ms), bandwidth scales with entity count (10 monkeys plus dozens of
  bananas), timing interactions like the catch need server-side rewind lag compensation, and
  replays and bot matches would need a separate deterministic path anyway.
- *Peer-to-peer GGPO rollback*: O(N²) connections for 10 players, NAT traversal pain, every
  peer is trusted, the session waits on the slowest peer or needs per-pair delay, determinism is
  required between every client build, and there is no authority to resync from. We keep
  rollback's zero-input-delay feel and add an authority.

Fairness property: a player's input is applied at the tick they intended, provided it arrives in
time. Boomerang flight depends only on past inputs, so your dash-catch is judged against the
exact banana you saw. Only the last ~RTT of remote inputs is uncertain, which is true of any
netcode model.

### 6.2 Timeline, clock sync and lead control

- Lead target: `lead ≈ RTT/2 + jitterMargin + 1 tick`. The server tags each acknowledgement
  with `arrivalMargin = inputTick − serverTick` (positive means early). The client runs its sim
  clock at 98–102% speed until the smoothed margin converges to +1..+2 ticks. Lead changes are
  gradual, except on initial sync and reconnect.
- Late input: the server has already used the predicted input; the client receives the
  authoritative frame, rolls back and sees itself corrected. The lead controller then increases
  the lead so it stops happening.
- Optional fixed input delay (0–3 ticks, default 0, per-match setting) trades a little local
  latency for fewer remote mispredictions, the same knob fighting games expose.

Lead control in numbers (v0.17, `kfm/net/clock_sync.hpp`, integer-only): RTT and jitter come
from `Ping`/`Pong` (every 50 ms until synced, then every 250 ms) with the RFC 6298 gains (1/8,
1/4), except that the jitter starts at 0 instead of R/2 so the first lead is not twice the RTT.
The initial sync waits for `Welcome`, the snapshot and three RTT samples, then sets
`L = estimated server tick + ceil((srtt/2 + 2·jitter)/tick) + 1 − inputDelay`, where the server
tick is extrapolated from the newest `Pong`'s tick by the time since it plus srtt/2. From then on
the smoothed margin (EMA, gain 1/8 per `AuthInputs`) drives the rate: 1.0 inside [+1, +2] ticks,
otherwise `1 − 1 % × (margin − nearest band edge in ticks)`, clamped to 98–102 %. Margin reports
about inputs sent before a sync or jump are ignored. A smoothed margin below −8 ticks jumps the
clock forward to the band's middle; the client never jumps backwards. The client's `TickClock`
accumulates ns × `kTickHz` × rate in ppm (one tick = 10^15 units, no drift) and returns at most 8
ticks per call. The input delay is 0–3 ticks (default 0): inputs passed at local tick L are for
tick L + delay.

The lead control at high RTT (v0.44, M6b step 4; `kfm_netsim --csv`, 60 s, 10 s warm-up, 10
clients; 4 clients agree within noise; jitter is ± per direction, uniform). The controller holds:
the rate never left 99–101 % (nowhere near the 98/102 % clamps; 1–8 % of seconds off 100 %), the
smoothed margin stays in its band, there are no resyncs, hard jumps or checksum mismatches, and
the lead follows RTT/2 (it sits 1–2 ticks below the formula's target, which counts 2 × jitter
that the margin band then trims). What it did not do was keep a jittery link's inputs on time:
with the fixed +1..+2 band the mean margin sat inside the band while the late tail of a ±30 ms
link was substituted, about 2.2 slot-ticks a second per client (≈ 4 % of ticks) at any RTT, since
the band ignores the spread. v0.44 makes the band jitter-aware (`kfm/net/clock_sync.hpp`): a
floor follows the margin reports half way down at each lower report and creeps back up by 2/256
of a tick per report (never above the smoothed margin), and the band moves up by how far that
lower envelope reaches below the mean beyond the band's low edge (at most 4 ticks). A steady
link keeps the +1..+2 band (100 ms / ±10 ms is unchanged); a ±30 ms link buys about 0.6 tick of
margin (10 ms of input latency) and its substitutions fall twentyfold. Server substitutions per
client per second, before → after:

| RTT | ±20 ms, 2 % | ±20 ms, 5 % | ±30 ms, 2 % | ±30 ms, 5 % | lead / target (ticks, ±30 ms, 2 %, after) |
|---|---|---|---|---|---|
| 200 ms | 0.20 → 0.16 | 0.50 → 0.38 | 2.25 → 0.12 | 2.62 → 0.31 | 8.0 / 10.0 |
| 300 ms | 0.20 → 0.16 | 0.50 → 0.40 | 2.19 → 0.10 | 2.73 → 0.34 | 11.0 / 12.9 |
| 500 ms | 0.18 → 0.13 | 0.50 → 0.36 | 2.15 → 0.11 | 2.61 → 0.31 | 17.0 / 18.9 |

What remains at 5 % loss is loss, not timing (an input lost beyond its redundancy). At 500 ms the
rollback depth (p99 34–35 ticks) exceeds the 32-tick ring, so most rollbacks re-simulate from the
verified anchor (about 11 k "rollbacks past the ring" per minute across 10 clients); nothing
overflows the 128-tick frame history. The soak gates gain a third, `netsim_10c_500ms_5pct`
(500 ms, ±30 ms, 5 %, 30 s; label `soak`), and `kfm_netcode_tests` pins the jitter case (200 ms,
±30 ms: under 0.4 substitutions a second, where the fixed band gave 2.2).

Client tick, in order:

```
1. net.pump()                       drain packets: auth frames, acks, snapshots, control
2. reconcile()                      any auth frame != predicted → load state, re-sim to L
3. in = sampleInput()               as late as possible before stepping
4. predicted[L].me = in; sendInputs(L, last 8..16 inputs)
5. saveState(L); step(world, predicted[L], events)
6. L++; presentation.render(interpolate(prev, curr), smoothingOffsets, events)
```

`NetClient` (v0.17) maps that onto one host frame: `beginFrame()`, `pump(now)`, `reconcile()`,
then `ticksDue(now)` times `tick(localInputs)`, then `onRollback` in presentation when
`frameReport().rolledBack`, then render `world()`/`prevWorld()` with `drainEvents()`. It keeps the
ring of 32 saved states (`memcpy`), the frames it stepped each tick with (128 ticks) and an anchor:
the state at the first tick whose frame is not yet known to be authoritative. A misprediction
inside the ring restores the saved state of its tick; an older one (a snapshot or a long burst of
loss put the prediction more than 32 ticks ahead of the authoritative stream) re-simulates from
the anchor, and only one older than the 128-tick frame history asks for a resync. Remote slots and
the present mask repeat the newest authoritative frame before the tick; local slots use the
inputs submitted for it. Inputs are resent back to the newest tick the server has assembled (at
most 16). Checksums are compared at confirmed ticks against the saved, anchor or current state of
that tick. A spectator (no slots) never predicts: it steps only authoritative frames and its clock
keeps it `spectatorDelayTicks` (3) behind the newest one. A snapshot it loads that is older
than the frames it already holds (the stream keeps arriving while a snapshot is delayed) is
followed, inside the load, by a run over those frames to that same delay (v0.76); it is part
of the load, not a hard jump. `sizeof(NetClient)` is about 360 KB
(the ring is 32 × 5.9 KB), so hosts allocate it on the heap.

### 6.3 Message layer

Our layer owns the messages, their bit-packing (`BitWriter`/`BitReader`), input redundancy and
clock sync. It asks two things of the transport underneath: an **unreliable, unordered lane**
(inputs and authoritative frames) and a **reliable, ordered lane** (control messages, snapshots).
GameNetworkingSockets, WebRTC data channels and the in-process loopback all provide exactly those
two lanes, which is why the game netcode never has to know which one it is running on. Each
message begins with a one-byte type; everything else is bit-packed and quantised.

| Message | Direction | Lane | Contents |
|---|---|---|---|
| `Input` | C→S, every tick | unreliable; last 8–16 inputs resent each packet | `startTick`, `count`, inputs (RLE-coded, they rarely change), last auth tick received |
| `AuthInputs` | S→C, every tick | unreliable; redundant | `startTick`, `count`, present-mask, inputs for all players, `arrivalMargin` for this client, state checksum every N ticks |
| `Snapshot` | S→C | reliable (transport fragments) | full `WorldState` via visitor plus tick (uncompressed: LZ4 is deferred, see below) |
| Control | both | reliable | Hello/Connect (token), ContentHash/BuildId, JoinMatch, PlayerJoined/Left, MatchConfig, Disconnect |
| Ping | both | transport stats plus our own tick-stamped ping | — |
| `ResyncRequest` (v0.17) | C→S | reliable | tick and client checksum of a mismatch; answered with a `Snapshot` |
| `RoomSettings` (v0.42) | C→S | reliable | the host's arena, mode, loadout, power-up set and bot count |
| `RoomState` (v0.42) | S→C | reliable | room phase, host slot, settings, human and bot slots, the bots' placement (v0.75), the player cap (v0.76), winner, nicknames |
| `Start` (v0.42) | C→S | reliable | the host starts the match, or plays again after it |

Redundancy replaces retransmission on the hot path, so a lost packet costs nothing unless 8+
consecutive packets vanish. A longer burst (an outage of the server-to-client direction past the
16-frame window) leaves frames no later message carries; the client repairs that gap with a
snapshot (v0.70, §6.6 "A gap in the authoritative stream"). Unreliable messages stay under 1200 bytes so they never fragment;
snapshots go over the reliable lane, where the transport fragments and reassembles them.

Bandwidth at 60 packets/s with 10 players, including transport overhead (GNS ≈ 30–50 bytes per
packet with its encryption tag, WebRTC ≈ 60–80 bytes with SCTP/DTLS): client upstream ≈ 4–7 KB/s;
server downstream per client ≈ 8–14 KB/s; one snapshot ≈ 5–20 KB. A 10-player match costs the
server well under 150 KB/s.

As built (v0.17, `kfm/net/messages.hpp`, `kProtocolVersion = 1`): every message is a one-byte
type tag followed by LSB-first bit-packed fields, zero-padded to a byte; decoders are fallible
without exceptions (`std::expected<void, DecodeError>`: truncated, wrong type, out of range,
trailing bytes, invalid state, checksum mismatch), reject non-zero padding and trailing bytes,
and are fuzzed with random and mutated buffers. The `Hello` type byte and version field are
frozen across versions so a server can always answer `Reject(ProtocolMismatch)`.
- Inputs are delta-coded against the slot's previous input (the first against idle): one
  "same" bit, else a 5-bit changed-field mask and 8 bits per changed field (46 bits worst
  case). `Input` carries `startTick`, a count of 1–16 (`kMaxInputRedundancy`), the client's slot
  mask (a client owns several slots in local multiplayer) and the last authoritative tick it
  received. `AuthInputs` carries the present mask once plus a "same mask" bit per later frame,
  an optional saturated `arrivalMargin` (i8) and an optional world checksum for `endTick − back`.
  Worst cases, asserted at compile time against `kMaxUnreliableBytes`: `Input` 931 B,
  `AuthInputs` 935 B (10 players × 16 ticks, every field changing); a steady 10-player
  `AuthInputs` is about 85 B, `Ping` 13 B, `Pong` 17 B.
- `Snapshot` is the seed, present mask, a `sim::checksum` of the world and the `WorldState`
  through the visitor; the reader checks both the visitor's validation and the checksum.
  Measured 4 740 B with every pool full and at most 1 308 B in a scripted 10-player match, so
  LZ4 (§10) is deferred: uncompressed is under 1/13 of `kMaxReliableBytes`.
- Control: `Hello` (version, `contentHash`, `buildId` (0 unless the build was stamped, §5.1
  v0.70), a token of up to 64 bytes, requested slot count with 0 = spectate), `Welcome` (assigned slots, `MatchConfig`, server tick), `Reject`
  (protocol, content or build mismatch, bad token, match full, shutting down), `JoinMatch`,
  `PlayerJoined`, `PlayerLeft`, `MatchConfig` (with its start tick), `Disconnect` (quit, kicked,
  timeout, protocol error, shutting down). `Ping{sendNs, tick}` / `Pong{echo, tick, holdNs}`.
- The room (protocol version 6, v0.42; §6.6): `Hello` ends with a nickname (length u8, at most
  16 bytes of well-formed UTF-8 without C0, DEL or C1 controls; empty = none). `RoomSettings` is
  arena u8, mode u8, loadout u8 (below their capacities), power-ups u16 and bots u4 (at most
  `kMaxPlayers`): 6 B. `RoomState` is phase u2 (Waiting, Playing, Over), an optional host slot
  (u4, a human slot), the settings, the human and bot masks (disjoint), an optional winner and a
  nickname (length u5) per human slot. `Start` is the type byte alone. Protocol version 7
  (v0.75) adds the bots' placement to `RoomState`, a third mask after the bot mask, disjoint
  from the humans: at most 178 B (`kRoomStateMsgMaxBytes`; the 191 B this paragraph gave
  before was never the constant's value). While the room waits it is the slots the settings'
  bots take if the host starts now; in Playing and Over it is the slots bots hold, equal to
  the bot mask (§6.6). Protocol version 8 (v0.76) adds the server's player cap after the
  placement mask, u4 holding the cap − 1: 1 to `kMaxPlayers`, with no human, bot or placement
  slot at or above it (refused on encode and on decode). `RoomState` is then at most 179 B.
- `kfm/net/input_history.hpp` holds the shared pieces: a 128-tick `SlotInputHistory` per slot
  with `getOrRepeat`, `InputHistory::makeInputMsg`, `FrameHistory::makeAuthInputsMsg` (a gap
  shortens the window) and the server's `InputFrameAssembler` (accepted / late / too early by
  more than 64 ticks / bad slot; `assembleNext` substitutes repeat-last and records it as the
  authoritative input, so a late arrival can never change a stepped tick).

### 6.4 Transport abstraction

```cpp
struct ITransport {
  virtual void sendUnreliable(PeerId, std::span<const std::byte>) = 0;  // inputs, auth frames
  virtual void sendReliable(PeerId, std::span<const std::byte>) = 0;    // control, snapshots
  virtual bool recv(Packet& out) = 0;                                   // carries a receive timestamp
  virtual void poll(ConnectionEvents&) = 0;                             // connect / disconnect / stats
  virtual ~ITransport() = default;
};
```

Implementations, in priority order:

- `LoopbackTransport` + `NetEmulator` (latency, jitter, loss, duplication, reorder): N clients and
  a server in one process. This is how the netcode is unit-tested and how the `netsim` app works.
  Every real transport must pass the same contract test suite as the loopback.
  As built (v0.17, `kfm/net/loopback.hpp`): a `LoopbackNetwork(seed)` hub owns the server
  endpoint and every client endpoint; each link direction has its own `Rng` seeded from the
  network seed, the client id and the direction, and every message consumes the same number of
  draws whatever the probabilities, so changing one link's conditions never shifts another's and
  a run is identical for the same seed and calls. `LaneConditions` per lane: latency, jitter
  (uniform, capped near 4.29 s), loss, duplication and reorder in ppm, a reorder hold-back and a
  retransmit delay. Unreliable copies arrive at send time + latency + jitter (+ hold-back);
  the reliable lane adds `retransmitNs` per lost attempt (at most 31) and never delivers before
  the previous reliable message on that direction, so it is exactly-once and in order.
  A send is stamped with the sender's last poll time; a message becomes readable on the first
  `poll(now)` at or after its delivery time, ordered by (delivery time, send order), and
  `receivedNs` is that delivery time raised to the receiver's previous poll. Connected is seen
  after the reliable latency on each side; `cut()` emulates a timeout on both. Client PeerIds
  start at 1 and are never reused; the receive queue holds 8192 messages. `setOutage(client,
  toServer, Outage{from, until, reliable})` (v0.70) blacks one direction out without
  disconnecting: unreliable messages sent in `[from, until)` of the sender's clock are lost, and
  with `reliable` the reliable lane's are delivered no earlier than `until` + its latency, in
  order, as a retransmission after the outage; the random draws are the same as without it.
  `Outage::unreliable` (default true) set to false holds the reliable lane alone and leaves
  the unreliable stream flowing: test support for the loss of one control message (a
  `ResyncRequest`, a `Snapshot`'s first attempt), which the reliable lane never drops and in
  steady play carries nothing else.
  The contract suite (`tests/unit/net/transport_contract.hpp`, target
  `kfm::transport_contract`) takes a harness (server, client, optional extra client, clock,
  advance, timeout) and generates eleven cases: connect, both lanes both ways, reliable order and
  exactly-once over 300 messages, both size limits with one byte over rejected, unknown peers,
  disconnect from either side, exact stats deltas, `recv` span rules, peer identity. It runs over
  a clean loopback and over a 30 ± 20 ms link with 20% reliable retransmits.
- `WebRtcTransport`, the single production transport. WebRTC data channels over DTLS/SCTP/UDP:
  one channel with `ordered: false, maxRetransmits: 0` for the unreliable lane, one ordered
  reliable channel for control and snapshots. The server and the native client use libdatachannel
  (C++); the browser build uses the same C++ API through datachannel-wasm, which wraps the
  browser's `RTCPeerConnection`, so the transport source is shared by all three. Encryption is
  built in. Clients connect out to a dedicated server with a public address, so ICE completes with
  host and server-reflexive candidates and no TURN relay is needed. Signaling (the SDP
  offer/answer) rides on the lobby service's WebSocket, which exists anyway for room codes and
  connection tokens.
- `GnsTransport`, optional and only for a Steam build: Valve's GameNetworkingSockets,
  API-identical to Steamworks' `ISteamNetworkingSockets`, which gains Steam Datagram Relay. Kept
  behind the same interface; the server can run it alongside WebRTC so Steam and browser peers
  share a match.

As built (v0.17), `kfm::net::WebRtcTransport` (`engine/net`, target `kfm_net_webrtc`):

- Channels: the client opens `"r"` (ordered, reliable) and `"u"` (`unordered`,
  `maxRetransmits = 0`) in-band; the server accepts them. A peer is `Connected` once both are
  open; a failed or closed peer connection, or either channel closing, is `Disconnected`, and so
  is a handshake that has not finished within `connectTimeoutMs` (10 s; on the server an
  unfinished peer is dropped silently because it was never announced). PeerIds are assigned
  1, 2, 3, ... when a client's signaling socket arrives and are never reused; at most
  `maxPeers` (≤ `kMaxPeers`) at a time.
- Limits: sends over `kMaxUnreliableBytes` / `kMaxReliableBytes`, to unknown or unconnected
  peers, or on the unreliable lane while more than 64 KB is already buffered are dropped and
  counted in `sendErrors`; oversize arrivals are dropped as `recvDropped`. libdatachannel
  advertises a 256 KB SCTP max message size (browsers advertise their own 256 KB).
- Signaling (M3): the dedicated server hosts its own plain `ws://` WebSocket
  (`rtc::WebSocketServer`, ephemeral port allowed); the lobby service takes it over later
  (decided in v0.37, §13.5: the server-side transport takes an injectable signaling source,
  the built-in socket stays for development and tests, and in production the lobby relays
  the same JSON messages over the server's outbound lobby connection).
  One flat JSON object per text message, `{"type":"offer"|"answer","sdp":...}` or
  `{"type":"candidate","candidate":...,"mid":...}`, hand-written encoder/decoder
  (`kfm/net/webrtc_signaling.hpp`). The client offers, the server answers, both trickle
  candidates; candidates that overtake the SDP are held until it is applied. No TURN; an
  optional STUN URL in both configs.
- Threading: libdatachannel's callbacks (its own threads natively, the main thread in the
  browser) only append to one mutex-guarded queue; `poll()` drains it on the caller's thread and
  does everything else there (creating and answering peer connections, adopting channels,
  timeouts), so one implementation serves all three targets. Arrival times are taken on the
  callback thread and mapped onto the caller's clock in `poll()`. `recv()` pops what `poll()`
  made readable; its span is backed by the transport until the next `recv()`.
- The `rtc::` API throws, so `kfm_net_webrtc` alone is built with exceptions (wasm exceptions in
  the browser) and catches at every entry point; its header includes nothing from `rtc::`, and
  `kfm_net` stays exception-free. The connection token rides in the client config for the message
  layer to send; the transport never validates it (§6.7).
- Lobby modes (v0.38, `docs/lobby-protocol.md`): `WebRtcServerConfig::externalSignaling`
  creates no WebSocket server; the host relays signaling through `acceptExternalPeer()`,
  `deliverSignaling()`, `takeOutgoingSignaling()`, `takeDroppedExternalPeers()` and
  `closeExternalPeer()`, with the same PeerId numbering, `maxPeers` and connect timeout as
  socket peers (callbacks still only enqueue; outgoing answers and candidates are queued and
  drained on the caller's thread). `WebRtcClientConfig::lobby` (`LobbyRequest`: create in a
  region or join a code, build id and content hash as 8 hex digits) makes `signalingUrl` the
  lobby's base URL (`/client` appended): the first frame is the request, the reply must be
  `joined` (the token becomes `connectionToken()`, `lobbyRoom()` the code and region, and only
  then the offer goes out) or `refused` (Disconnected, `lastError()` "lobby refused: reason");
  the socket stays open for the transport's lifetime and its close is always Disconnected. The
  envelopes are flat JSON objects of strings, read by the same hand-written decoder
  (`signaling::decodeObject`/`encodeObject`). `NetClient::setToken()` hands the lobby token
  over before the first `pump()` after `joined`. `LobbyLink` (`kfm/net/lobby_link.hpp`, native,
  in `kfm_net_webrtc`) is the server's socket to `<lobby>/server`: register, peers and tokens
  (`acceptsToken()` is `ServerConfig::acceptToken`), relay both ways, `peer_gone` for peers the
  transport dropped itself, `capacity`, and a reconnect with backoff (1 s doubling to 30 s,
  reset once registered; a `refused` registration is retried the same way, the process stays
  up). Tested in `kfm_lobby_tests` (label `network`) against an in-process fake lobby and in
  `kfm_net_webrtc_tests` by relaying between two transports by hand with no socket at all.

Why WebRTC everywhere rather than GNS natively plus WebRTC for the browser: with the browser as
the primary client there is exactly one transport in production, one code path to harden, and
native clients cross-play with browsers without server-side bridging. The per-packet overhead
(≈60–80 bytes) is irrelevant at our packet sizes. The latency is UDP latency.

Why not WebSocket: TCP, so one lost packet stalls everything behind it. Unsuitable for the hot
path, fine for signaling.

WebTransport (QUIC datagrams plus streams) maps onto our two lanes even more directly than WebRTC
and needs no ICE or SDP, only a URL and a certificate hash. It is the likely successor once every
browser we care about ships it (Safari was last to add it; verify before depending on it) and a
C++ server library is mature. The seam makes that swap a contained change.

### 6.5 Threads

- Server (M3 as built, v0.17): `kfm_server` (`apps/server`) hosts one match per process:
  `ServerHost` drives `NetServer` over a `WebRtcTransport` server from a drift-free 60 Hz clock
  (tick k is due at base + k·10⁹/60 ns; a frame runs at most 8 ticks and a larger backlog is
  dropped, counted and rebased), pumping every millisecond; it fills `--bots N` slots, announces
  the next match 3 s after `MatchEnded` with a splitmix64-derived seed and the same roster,
  prints one log line per join/leave/kick/resync/round/match and a periodic delta stats line,
  and on SIGINT/SIGTERM or an exit limit shuts down (300 ms linger for the reliable lane).
  `--rounds-to-win` is deliberately absent: it would change the content hash and reject every
  client; a different rule set is a mode in the content, chosen with `--mode` (v0.18). The
  multi-match host below is the target shape; M6a ships one process per match under a
  systemd pool and only builds `MatchHost` if a VM runs out of processes before it runs out
  of CPU (§13.5, v0.37). As built (v0.38): `kfm_server --lobby URL --lobby-secret S --region R
  [--name N]` (the secret also from `KFM_LOBBY_SECRET`, the name defaults to hostname:pid,
  exclusive with `--port`/`--bind`/`--token`, no TCP port bound) runs the same `ServerHost`
  with a `LobbyAdapter` (`apps/server/server_lobby.hpp`) beside it: the token check is the
  link's token set, the stats line ends with the lobby state, a room assigned while no peer is
  connected restarts the match through `ServerHost::startNewMatch` (the bots are re-seeded;
  v0.42: `ServerHost::openRoom`, which in the default room flow resets the room to Waiting with
  the server's defaults instead, §6.6), and the capacity sent is max players minus the bots
  placed, updated only when that total changes (a waiting room places none, v0.42).
  `--ice-ports A-B` and `--stun` apply in both modes (the VM firewall opens the range; GCP's
  1:1 NAT needs the STUN-derived server-reflexive candidate). `ServerHost` itself still knows
  nothing about the lobby and its loopback tests are unchanged.
- Server (target): `MatchInstance` = world + `NetServer` + bots. A `MatchHost` process runs many
  matches on a thread pool (the sim is cheap). One socket I/O thread timestamps arrivals and
  pushes to a single-producer single-consumer queue per match.
- Native client: main thread (input → sim → render submit); libdatachannel's own service threads
  hand packets with receive timestamps to a queue; SDL's audio thread. Rendering can move to its
  own thread later.
- Browser client: everything on the main thread inside the `requestAnimationFrame` callback, which
  works because the sim is single-threaded by design and WebRTC callbacks arrive on that thread.

### 6.6 Join, reconnect, resync

- Checksums are compared at confirmed ticks. Mismatch → request snapshot → hard load →
  continue predicting. Dev builds dump both states for diffing.
- Late joiners and spectators receive a snapshot at tick T plus the authoritative input stream
  from T onward. Spectators are clients without a player slot.
- Reconnect within a grace window reuses the reserved slot through the same flow.
- A joiner's slot is present in the authoritative frames from the tick after its `Welcome`; it
  spawns at the next round start (§5.4 roster, v0.17).
- `NetServer` (v0.17) is driven by its host: `pump(nowNs)` drains the transport between ticks,
  `tick(nowNs)` assembles, steps and broadcasts one tick; it owns no clock. Handshake: `Hello` on
  the reliable lane is checked for protocol version, content hash, build id, token (a host
  callback; empty accepts all) and room, then answered with `Welcome` (slots, `MatchConfig`,
  server tick), a `Snapshot` of the current world and a `PlayerJoined` broadcast, or `Reject`.
  Content negotiation (v0.18) is the content hash in `Hello`: equal hashes mean identical
  arena and mode lists, so the `MatchConfig` in `Welcome`, `Snapshot` and `MatchConfig`
  (seed, roster, arena index, mode index; protocol version 2; the power-up set as a u16 since
  protocol version 3, v0.24; the loadout index as a u8 below `kMaxLoadouts` since protocol
  version 4, v0.31; the team of every slot, 4 bits each and below `kMaxPlayers`, since protocol
  version 5, v0.32) is all a client needs to simulate the server's arena, mode, power-ups,
  loadout and teams. A client steps with the config of the snapshot it last loaded
  (`NetClient::simConfig()`); an announced restart's config (`matchConfig()`) takes effect
  with the new match's snapshot, so a restart into another arena costs no resync.
  Slots are the lowest free ones; `requestedSlots = 0` is a spectator; `JoinMatch` grows or shrinks
  a peer's slots later. Server-side bots are slots the host marks with `setBot()`; they get
  `scriptedInput` every tick (§8).
- Each `AuthInputs` carries the frames the peer has not acknowledged (its `lastAuthTick` from
  `Input`, at most 16), the minimum arrival margin of its inputs since the previous send (§6.2) and,
  every `checksumEveryTicks` = 10 ticks, the checksum of the world after the newest frame.
- Checksum mismatch: the client sends `ResyncRequest{tick, its checksum}` (reliable); the server
  answers with a fresh `Snapshot`, at most once per 250 ms per peer.
- A gap in the authoritative stream (v0.70): `AuthInputs` resends at most 16 frames, back to the
  peer's acknowledgement, so a server-to-client outage longer than about 16 ticks (shorter than
  the 5 s timeout) leaves frames that no later message carries. Before v0.70 nothing repaired
  that by design: the client's confirmed and verified ticks stuck at the gap, so no checksum was
  compared again, a player predicted on unverified until a misprediction fell out of the 128-tick
  frame history (≈ 1.8 s after the outage, the rollback-overflow resync), and a spectator, which
  steps only authoritative frames, froze for good (`tests/netcode/outage_test.cpp` reproduced all
  three for 0.4, 1 and 3 s outages). The repair is explicit and client-side, so it covers
  spectators, which never acknowledge anything: the client keeps the latest first tick of the
  `AuthInputs` windows it has received (the server's windows only move forward); once that is
  past the first frame it lacks (`confirmed + 1`), no later message will bring that frame, and
  after `ClientConfig::gapResyncNs` (150 ms, for copies reordered behind the window) it sends a
  `ResyncRequest` like a checksum mismatch does (the 250 ms server limit and the 1 s client retry
  apply; `ClientNetStats::gapResyncs` counts them). Not while it holds for the room: the resumed
  match's snapshot is coming anyway. The snapshot loads as any resync; frames after its tick are
  in the client's 128-tick frame history or still being sent, since the server's window moves on
  from its tick. Measured over a 40 ± 5 ms link: the client asks about 200 ms after the outage
  ends and agrees again (frames confirmed to within a few ticks of the server, checksums compared
  and matching, a spectator on the server's state) about 0.3–0.4 s after it, for 0.4, 1 and 3 s
  outages, of one lane or both; a 150 ms outage (inside the window) needs no snapshot. The
  rollback-overflow resync stays as the backstop. The emulator gained `LoopbackNetwork::setOutage`
  for this (§6.4). The same outages are covered at the high-RTT conditions of §6.2
  (`tests/netcode/outage_test.cpp`): at 200 ± 30, 300 ± 10 and 500 ± 30 ms RTT the clients agree
  again 0.5–0.6, 0.6–0.8 and 0.9–1.1 s after the outage ends, each inside a bound the test
  derives from the mechanism (three one-way delays, `gapResyncNs`, one checksum period and a
  tick). There the backstop does part of the work: once the redundancy window, one hop and the
  client's lead add up to the 32-state ring (a 3 s outage at 300 ms, 1 s or more at 500 ms), the
  first frames after the outage are a rollback overflow, and that request goes out on the first
  window without waiting `gapResyncNs`. `ClientNetStats::rollbackOverflows` counts such
  episodes (v0.76): once when the client starts waiting for a snapshot it cannot do without,
  and not again until one has loaded, so a repair through the backstop counts one. Also covered: 2 to 10% loss on top of the outage; the
  `ResyncRequest` or the snapshot held back (a request held past the client's 1 s retry is
  retried and the server's 250 ms limit drops the duplicate; a snapshot held that long is
  served twice and both load, in order, the spectator back at its delay in the frame of each
  load); and a second outage that starts before, or just
  after, the first repair completes, which costs a second snapshot and no more. The server's lead estimate (§8 human-likeness) also counted the outage: a stale
  acknowledgement reads as a round trip growing one tick per tick, and before v0.70 a 3 s outage
  pushed that peer's estimate to 146 ticks (`humanLeadTicks()` clamps to 60, so a mixed match's bot
  delay went to its clamp); it now samples only messages whose acknowledgement moved on.
- Reconnect: a peer that drops (transport close or 5 s without a packet) while holding slots and a
  non-empty token leaves them reserved for `reconnectGraceTicks` (10 s): still present, fed an idle
  input (not "repeat last", so a held stick does not run the monkey into a wall). A `Hello` with
  the same token reclaims them and gets a fresh snapshot; a `Hello` whose token belongs to a
  still-connected peer takes its slots over (the client noticed the drop first). After the window,
  or on `Disconnect{Quit}`, the slots leave the roster (`PlayerLeft`).
- Match restart (v0.17): `NetServer::restartMatch(seed, startTick, botSeed)` (keeps the arena
  and mode; v0.18 adds `restartMatch(MatchConfig, startTick, botSeed)` to change them) sends
  `MatchConfig{config, startTick}` at once to every peer (and to anyone who joins before
  `startTick`, after their snapshot); the message means "the world from `startTick` is a new
  match whose snapshot follows". At `startTick` the server's world becomes `initialWorld` with
  its tick set to `startTick` (ticks never go backwards, so client clocks, frame histories and the
  assembler carry over; this is the one place outside a sim system that writes a `WorldState`
  field, and it is safe because the sim only uses tick differences), every peer gets a
  `Snapshot`, the bots are re-seeded and a `MatchRestarted` host event fires. The client drops
  server checksums for ticks at or after the announced start until that snapshot loads, then
  hard-loads it like a resync (`FrameReport::matchRestarted`), so an announced restart costs no
  resync; an immediate restart under reliable-lane loss can cost one, which is why the host
  announces 3 s ahead. `shutdown()` sends `Disconnect{ServerShuttingDown}` to every peer and
  rejects later `Hello`s; `closeAll()` then drops them. `ServerConfig::maxPlayers` caps the slots
  open to peers and bots.
- Restart per mode (v0.23): every mode ends in `MatchEnded`, so `kfm_server` and the local
  `MatchSession` restart the same way whatever the mode: a deathmatch ends on `scoreToWin` or
  its clock (one round, so `round` stays 1), survival and rounds on `roundsToWin`; a draw
  (deathmatch's tied lead) is a `MatchEnded` with an invalid winner, logged as "drawn". The
  restart keeps the mode, and `initialWorld` starts the next match with zeroed mode counters,
  so nothing (scores, deaths, respawn timers) carries over. Since v0.32 `kfm_server` restarts
  with the teams rebalanced over the slots present at that moment (`sim::teamsFor`), through
  `restartMatch(MatchConfig, …)`, and logs the winner's team in a team mode. A deathmatch joiner does not wait for
  a next round that never comes: its slot spawns `respawnTicks` after it appears in the frames
  (§5.3), at the farthest spawn point.
- The online room (v0.42, M6b; `apps/server/room.hpp`, `ServerHost` with `HostConfig::room`):
  room state lives in the host beside `ServerHost`, never in `WorldState`, and `NetServer`
  keeps no policy (it reports `RoomSettings`/`Start` as events and sends the `RoomState` the host
  sets, to every peer at once and to each joiner after its `Welcome` and `Snapshot`). Three
  phases. **Waiting**: no match runs. The sim leaves `MatchPhase::Lobby` on its first step, so
  it cannot idle: the host pumps the `NetServer` but never calls `tick()` (the server tick does
  not move, the host clock is rebased every frame so the first tick after Start runs no
  backlog), peers join, see the roster, settings and host, and `releaseReservations()` frees a
  leaver's slot at once (no tick would expire it). **Playing**: as before M6b; a joiner
  spectates until the round break or spawns after `respawnTicks`. **Over**: entered on
  `MatchEnded`; again no tick, the clients show the final world and the winner. Transitions:
  the host's `Start` in Waiting places the bots and calls `restartMatch(config, nextTick(),
  botSeed)` with the room's settings (next seeds as after a match end, teams by `teamsFor`);
  its `Start` in Over removes the bots and returns to Waiting with the same settings ("play
  again"); the room returns to Waiting when its last human slot is gone, and `openRoom()` (a new
  lobby room with no peer) resets it to the server's defaults. The host is a slot, not a peer:
  the first human slot to appear, then the longest-present one left, none without a human; a
  slot reserved for a reconnect stays a member, so a host who drops mid-match keeps the role,
  while outside Playing reservations are released and the role passes at once. Settings are
  validated against the content (indices in range, `ContentDB::compatible`, bots ≤ max players
  − 1); an invalid set, a non-host's request or a request outside its phase is ignored and the
  current `RoomState` re-sent; at Start the bots are clamped to the free slots. One function
  decides those slots, `server::roomBotSlots(free, count)` (the lowest `count` free slots under
  the server's player cap, all of them when fewer are free): Start places the bots with it and
  the Waiting `RoomState` carries its answer as the bots' placement (v0.75), re-sent like any
  other change on a join, a leave or a settings change, so what the waiting room previews is
  what Start does. The client has no copy of the rule. Every `RoomState` also carries the
  server's player cap (v0.76): the waiting room ends its slots at the cap, and the host's Bots
  row stops at the cap − 1, where the validation above does.
  The client side of a server that does not tick: `NetClient` holds while its `RoomState` says
  Waiting or Over (`holding()`: `ticksDue()` returns 0 and restarts the clock, `tick()` sends
  nothing), so no input runs ahead of a frozen assembler and nothing accumulates; pings go on
  (both 5 s timeouts stay fed, the RTT estimate stays fresh). When the room turns Playing, the
  client keeps holding until the restart's `Snapshot` has arrived and a `Pong` reports a server
  tick past it, then hard-loads it and runs forward to estimated server tick + target lead with
  the lead controller and clock reset, exactly as the initial sync: the lead never carries the
  pause over. A client of a server that sends no `RoomState` (the M3 policy, `kfm_netsim`, the
  netcode tests) never holds.
- Bots yield (v0.44, M6b): in the room flow `ServerConfig::queueForBotSlots` is on, so a `Hello`
  (or `JoinMatch`) that wants more slots than are free, but no more than are free or held by
  bots, is answered with a `Welcome` of no slots instead of `Reject{MatchFull}`: the peer
  spectates and waits (`ServerEventKind::PeerQueued`; a request bigger than that is still
  `MatchFull`, and without the flag nothing changes: the M3 policy, `kfm_netsim`). The host
  decides when a bot gives its slot up and hands free slots to the oldest waiting peer with
  `NetServer::grantQueued()`, which sends `PlayerJoined{yours}` like any join (all of a
  request or nothing). Outside Playing (Over; Waiting places no bots) the highest bots go at
  once. While Playing a bot yields when its monkey is off the field: eliminated (waiting for a
  respawn, or for the next round in a rounds mode), or at a `RoundEnded` (then any bot); the
  highest such bot is removed (its agent dropped from the `AgentHost`) at that tick and its slot
  is granted one tick later, so the sim sees a leave and a later join and never an owner change
  of a live monkey: a respawning mode forfeits the bot's score and deaths and spawns the joiner
  `respawnTicks` later like any late joiner, a rounds mode spawns it at the next round start,
  a team mode gives it the bot's team. Any slot that comes free (a human leaves, a reservation
  expires) goes to the oldest waiting peer as well. A yielded bot does not return within the
  match: `RoomSettings.bots` means "bots when there is room", applied (clamped to the free
  slots) at every Start, which keeps the rule to one place. Since every bot can yield, the room
  flow's lobby `capacity` is max players (`LobbyAdapter`; the M3 flow still offers max players
  minus its bots). The client: a spectator that receives `PlayerJoined{yours}` while running
  (`NetClient::waitingForSlot()` before it) runs forward on its next `ticksDue()` from its
  delayed position to estimated server tick + target lead, predicting, with the lead controller
  and the clock reset (counted as a hard jump); `NetSession` binds its devices to the new slots.
- Reconnect from the UI (v0.44, M6b): the token flow above behind a button. Since v0.76 a
  lobby rejoin refused `unknown_code` is retried by the client on its own, 2 s apart and up to
  4 times, with the screen still on "Reconnecting": after a lobby restart the room's server
  may not have claimed the room back yet (§13.5, `docs/lobby-protocol.md` §3). A `NetSession`
  that ran and then lost its link (`Disconnected` by a timeout or a closed transport, or the
  lobby's `closed`/`kicked`, which is how a server-side timeout reaches a lobby client; not a
  quit, a kick, a shutdown, a closed room or a gone server) `canReconnect()`, and the network
  status screen shows "Connection lost" with Reconnect and Leave (keyboard, pad or mouse). The
  app builds a new transport for `reconnectConfig()`: the `--connect` server with the token
  the session used, or a lobby rejoin of the same room under the lobby's token (lobby protocol
  v1.2, §13.5); `NetSession::reconnect()` starts the `NetClient` over it (`connect()` on a new
  transport with the same token, as §6.6 always intended) and keeps the devices, nickname and
  room code. Within the grace window the server's reservation hands back the slots, and with
  them the nickname and, mid-match, the host role; after it, or outside Playing (no
  reservation is kept there), the client joins like anyone (a free slot, a bot's slot, or the
  queue). Both sides time a silent link out after 5 s, so of the 10 s grace about 5 s are left
  for the button; the screen says the place is kept "for a few seconds".

### 6.7 Authority and abuse resistance

- The server accepts only inputs, validated for range, rate (≤ tick rate plus slack) and tick
  window. There are no "I hit you" messages; nothing the client says about state is trusted.
- Connection tokens issued by a lobby service so only matched players can connect; the token
  rides in the first reliable message. GNS and WebRTC encrypt in transit. Rate limits per address.
  Message decoders are fuzzed.
- Aim assistance cannot be prevented server-side; acceptable for a party game.
- `NetServer` numbers (v0.17): inputs are stored only for slots the sending peer owns (foreign
  slots are ignored and counted), only with defined button bits (Throw..Taunt), and only for ticks
  in the assembler window (up to 64 ticks ahead; copies of assembled ticks are redundancy, not
  errors). `Input` messages pass a per-peer token bucket of `kTickHz + 30` messages per second with
  a one-second burst; the rest are dropped and counted. Undecodable messages, messages before the
  handshake, foreign slots, bad buttons and too-early ticks are protocol errors; more than 32 in
  10 s get `Disconnect{ProtocolError}` and the slots are released at once.
- The room (v0.42): `RoomSettings` and `Start` pass a per-peer bucket of 20 per second
  (`roomMsgsPerSecond`; excess ones dropped and counted); before the handshake they are protocol
  errors like any other message. Only the host's requests take effect, decided by the host's
  `Room`, never by `NetServer`.
- v0.70: `JoinMatch` passes its own per-peer bucket of 2 per second with a one-second burst
  (`joinMsgsPerSecond`; excess ones dropped and counted in `joinRateLimited`), since every one
  that changes the roster fans `PlayerJoined`/`PlayerLeft` out to every peer and spawns or
  removes monkeys. A connected peer that has not had a `Hello` accepted within 10 s
  (`helloTimeoutNs`) is disconnected (`helloTimeouts`): `Ping` is answered before the handshake
  (the client measures its RTT while it waits), so without a deadline a peer that only pings
  held one of the `kMaxPeers` entries for good. The client in turn never believes a server
  tick blindly: a snapshot load (initial sync, resync, resume) or a promotion runs forward at
  most `kMaxRunForwardTicks` (5 s) past the snapshot's tick, whatever the `Pong` tick said
  (`runForwardClamped`); a client cut short lags and catches up through the lead controller's
  forward jump (§6.2).

### 6.8 Perceived latency budget (local actions)

- Input is sampled immediately before `step`: ≤ 1 tick of staleness (8 ms average). The press
  time is still known exactly through the sub-tick stamp, so timing judgments do not inherit
  this jitter.
- Prediction applies the input in the same tick.
- Render interpolates between the previous and current tick (≤ 1 tick) or renders the newest
  tick directly when display rate ≥ tick rate. Natively, use a low-latency present mode and avoid
  deep swapchain queues. M2 numbers (v0.15): an entity is interpolated only when the same
  `EntityId` (index and generation) exists in both states; otherwise, or when it moved more than
  2.0 units between the two ticks (a dash moves about 0.37 per tick), it snaps to the newest
  state, its body yaw resets and any smoothing offset is cleared. The local client's fixed-step
  clock accumulates time in units of ns × `kTickHz` so a tick costs exactly 1e9 units (no drift
  at 1×; the time scale, clamped to 0.25–2× (0.25–4× since v0.68, for the replay viewer; local
  play's overlay slider still stops at 2×), scales the units added per frame); at most 4 ticks
  run per frame (adjustable 1–4; the replay viewer sets 8) and any further backlog is dropped
  and counted, never simulated late. Inputs for tick i are sampled over the wall-clock window that tick covers,
  immediately before its `step()`; the windows of a frame's batch end at the frame's own
  timestamp (the time up to which events were read) and count back in whole ticks, so the
  leftover accumulator only sets the render alpha and never pushes a press past its tick's
  end. Pausing freezes the accumulator and discards pending presses, and a Throw or Dash
  pressed during the pause and still held reads as released until it is let go, so nothing
  fires on resume or on a single step; a new match, a restart or a return to the lobby clears
  the pause and any pending single step, and a return to the lobby also resets the
  presentation state. v0.46: the pause menu (Escape) holds the same pause while it is open;
  online it pauses nothing and the local slots send idle input instead (§7). Render policy "auto": the newest tick is drawn directly when the
  measured display rate (EMA of frame intervals) is at least 0.95 × `kTickHz` × time scale (the
  tolerance covers 59.94 Hz displays), otherwise frames interpolate with
  `alpha = accumulator / tickCost`; the overlay can force either.
- Browser specifics: request the WebGL context double-buffered, without `desynchronized`
  (v0.56; v0.15-v0.55 asked for it to bypass Chromium's compositor queue, but it lets the
  display read the canvas mid-frame, and a frame that starts with a clear then flickers; v0.55's
  `preserveDrawingBuffer` did not cure it and went with it), read input and step the sim at the
  top of the `requestAnimationFrame` callback, render the newest predicted tick directly rather than
  interpolating, and keep no other compositing layers over the canvas. Expect the browser to cost
  one to two frames over a tuned native client; the netcode hides network latency, not this.
- Target glass-to-glass for your own actions: ~30–50 ms native, ~50–70 ms in the browser, on a
  60 Hz display and independent of network RTT. Remote actions appear ≈ RTT + margin late, smoothed.

### 6.9 Smoothing of corrections

After a rollback, each visible entity records `visualOffset = oldRenderPos − newSimPos` and
decays it exponentially over ~100–150 ms, snapping instead when the error exceeds a threshold
(teleports, dashes). The local player snaps or blends very briefly. The remote-player
"warp" per misprediction is small: a direction change hidden for ~4 ticks moves a monkey a few
pixels. M2 numbers (v0.15): offsets are keyed by pool index and generation and decay as
`exp(−dt/τ)` with τ = 120 ms for remote entities and 30 ms for local players; a correction over
1.5 units snaps; offsets below 1e-3 are cleared. Offsets move only the model: the blob shadow
stays on the interpolated sim position, which is the hitbox. `onRollback()` exists from M2 and
is only exercised by tests until M3.
As wired in M3 (v0.17, `apps/client/core/net_session.cpp`): `onRollback` is called right after
`reconcile()`, before the frame's new ticks, with the states and alpha the previous frame drew,
so the offset is the correction alone. Called after the ticks it would also absorb the frame's
own motion, and with a rollback on most frames (bots mispredict about 60 ticks/s) that compounds
into a standing lag of roughly 1/(1 − e^(−dt/τ)) frames, about 120 ms for remote players. A hard
clock jump inside the tick batch gets a second call with the new states. Presentation draws only
the events the journal delivers (§5.9), never the raw per-tick sink; retracted events are counted
and not yet undone on screen.
What is shown online (v0.24): until the match runs, the status window replaces the lobby; once the
server's `Welcome` arrived (`NetSession::configKnown()`) it adds the server's `MatchConfig`
read-only: the slot cards (you, online, free), the arena with the same preview as the local lobby,
the mode with its key numbers and the power-up set. After `MatchEnded` the banner adds the
announced next match (arena, mode, power-ups) from `NetClient::matchConfig()`. The browser page
with `?connect=` runs the same code and shows the same. Nothing of it is editable on a client.

---

## 7. Presentation (`engine/presentation`, `engine/platform`)

The sim is strictly 2D. Presentation draws it as real-time low-poly 3D: a fixed tilted camera
over the arena, skinned character models standing on the ground plane. Sim `(x, y)` maps to
world `(x, z)`; world `y` is height and is never read back into the sim.

- **Camera and projection.** One fixed camera per arena (perspective with a long lens, or
  orthographic; tuned in M2), looking down at the plane. No camera rotation in gameplay, so the
  tilt is part of the arena's design. A real depth buffer resolves overlaps; only transparent
  effects are sorted.
- **Arena statics (v0.22).** `makeArenaCamera(arena, ...)` frames each arena from its own
  `halfExtents` (fixed pitch and field of view, distance solved per arena and viewport), so a
  36 × 20 `corridors` sits further away than the 32 × 18 `dojo`; interior walls are drawn below
  `framingHeight` and never leave the frame. `buildArenaPieces()` turns the arena's walls and
  hazards into `RenderState::arenaPieces` (world-space boxes, walls first, then hazards, then
  pit rims); the renderer draws each as the unit cube scaled to it: walls as
  `kArenaWallHeight` (1.2) stone blocks, above a banana in flight so a bounce reads as hitting
  one; pits as a near-black slab just above the floor with a brown rim strip
  (`kPitRimWidth`) along every stretch of edge that meets open floor (adjoining pit boxes and
  the outer wall get none, so a ring of boxes reads as one pit); slow patches as a mud slab.
  Slabs stay under the blob shadows. Colours are `kArenaWallColor`, `kArenaPitColor`,
  `kArenaPitRimColor`, `kArenaSlowColor` in `palette.hpp`, sizes in `tuning.hpp`. (v0.49: the
  colours are the arena look's, below; a pit is water with a bank strip, a wall mossy rock.)
- **Look (v0.49).** The art direction is Jungle Toon: a Saturday-morning cartoon in a jungle
  clearing. Rules every theme keeps: players pop first (the floor is mid-value and muted; the
  player colour is large and saturated); the banana is the brightest object on screen and always
  outlined; hazards read by shape (a pit is always a drop, mud always bubbles, a wall always
  stands tall), so a theme changes the dressing, never what a thing means; looks are
  presentation only. As built: every monkey, banana, wall, rim block, power-up and carried flag
  has a dark-brown ink outline, an inverted hull (the mesh again with front faces culled, pushed
  out in world space by a fixed width: along the normal for smooth meshes, along each box axis
  for cubes so the corners stay closed); a floor slab (pit, mud) sits on a lower ink slab
  `kOutlineSlab` wider instead, since a hull's sides would hide under the floor. Shading is two
  cel bands (lit, and `kShadeBand` × a cool `shadeTint` below `kCelBandEdge` on n·l,
  anti-aliased with `fwidth`). `lit` draws surfaces from the world position so patterns never
  stretch with a box's scale: grass (two-tone blotches, small tufts, the faint unit grid, the
  darker band along the walls), water (deep patches and drifting crest lines), mud (blotches and
  bubble rings) and mossy rock (moss blotches on top faces). The colours are one `ArenaLook`
  (`palette.hpp`: `kJungleLook`, from the style board's swatches); the client's arena preview
  draws from the same look. The clear colour around the arena is the canopy green. The
  character (v0.50): the rig's vertex colours (`COLOR_0`, rgb the part's colour, alpha the
  player-colour mask) give brown fur, a pale face and hands, dark eyes, and the slot's colour on
  the gi and the headband (a torus with two tails); the head is big (0.75 of the 3.2-radius
  height) and the top of the head stays at the capsule's height. An eliminated monkey fades 0.8
  toward grey as a whole. Bananas have brown tips. The UI (v0.51) is the style board's Mango
  Sunset menu: bark-brown panels, ink borders, sand buttons that turn mango under focus with a
  chunky ink drop shadow, gold titles (`apps/client/ui_style.hpp`). Highlight (v0.55): one rule
  for every menu screen, in the shared widgets (`ui::showsFocus`). In mouse mode (at startup, and
  after any mouse motion or click) a button or row is highlighted only while the pointer is over
  it, and the hovered item takes its model's focus; a touch counts only while the finger is down,
  because ImGui keeps a tap's position. A keyboard or pad menu press switches to keys mode, where
  the model's focus is highlighted and a resting pointer is not; the press that switches only
  reveals the focus (no move, no activation) unless the pointer was on the focused item, and
  Enter as the lobby's or room's labelled Start shortcut is never held back. `MenuHighlight`
  (`apps/client/core`) holds the mode, `main.cpp` feeds it, the screens only read it. A menu
  screen is auto-height, so ImGui hides it for the frame it is created in while it measures
  itself; the client does not present that frame (v0.55: `platform::debugUiMeasuring()`,
  `commitWithoutPresent()`), so the previous picture stays up for one frame, never two in a row
  and never on the last frame of `--exit-after-frames`. Arena themes (v0.52,
  `theme.hpp`): Jungle, Temple and Beach `ArenaLook`s, each naming its floor, wall, pit and
  slow-patch surfaces; the player picks Auto (the arena's suggested theme) or one theme
  everywhere, locally, and nothing of it leaves the client. Props (bamboo clumps, stalls,
  treehouses) would be procedural meshes per theme, a later step.
  v0.54 supersedes parts of the above (changelog v0.54 has the detail): the arena plane is the
  `ground` program (flat colour, distance-field edges, rounded land with a cliff, no slabs, no
  wooden frame, a backdrop per theme, the unit grid only in the grid space); walls are rounded
  blocks; the monkey has the board's proportions (head 0.88 of the radius, body inside the
  hitbox, a painted face) and glides in idle while moving (`kMonkeyRunCycle`); buttons are
  coloured by role with a ring for focus.
  `kfm_render_probe --arena NAME` (and the client's `--arena`) shows any arena.
- **Height is cosmetic.** Banana arcs, dash hops and knockback lift the model along world `y`,
  but the sim position never leaves the plane. Every airborne thing casts a blob shadow (a decal
  quad on the ground) at its true sim position, and the shadow is where the hitbox is. This
  keeps the catch readable: you dash at the shadow. As built (v0.34): a monkey's shadow is its
  radius stat (`Stats::radius`, 0.35 under the grid baseline) and its model scales by the stat
  over the def's radius; a banana's shadow is the def's radius × its `radiusMul`. A mode's
  baseline (a permanent `ModifierList` entry) draws no timer pie. The grid needs nothing else:
  the interpolated slide reads as a hop from cell to cell.
- **Facing.** Characters rotate smoothly. Upper body follows the aim angle, lower body the move
  direction, blended in the animation layer; the throw direction, the aim indicator and the
  body always agree. This is the main readability gain over direction-quantised sprites.
  v0.48: while a throw charges (`PlayerFlag::Charging`), the aim bar stretches from its resting
  length to `kChargeAimLength` and warms toward gold with the charge
  (`RenderPlayer::throwCharge`). v0.53: the monkey winds up instead of a ground ring: the
  `charge` clip is sampled at `throwCharge × duration` (`AnimClip::Charge`, above the throw and
  below a dash in the clip priority), a full charge adds the strain tremble
  (`kStrain*` in `animation.hpp`), and the held banana sits in the skinned pose's `hand_r`.
  Every monkey's charge is drawn: it is a tell, as a wind-up should be.
- **Asset pipeline.** Characters, bananas and props are modeled, rigged and animated in Blender
  and exported to glTF by a script in the repo (one command regenerates every asset). Low poly
  counts and few joints per character (well under WebGL2 uniform limits for skinning). Per-player
  colour is a material parameter, not a texture variant. Source `.blend` files are checked in;
  exported glTF is built by the pipeline.
- **Renderer.** sokol_gfx: static meshes for the arena, GPU-skinned meshes for characters
  (joint matrices in uniforms, dual quaternions if seams show), instanced bananas and pickups,
  one directional light with flat or toon shading, blob-shadow decals, a few render targets for
  post (bloom, hit flashes, screen shake). No shadow maps and no dynamic lighting beyond the one
  light. Backends: OpenGL/Metal/D3D11 on desktop, WebGL2 (WebGPU later) in the browser. SDL3
  handles window, input, gamepads and audio on every target including Emscripten. SDL_GPU was
  the other candidate; at the time of writing it has no stable browser backend, which decides it.
- **Animation.** Skeletal clips (idle, run, dash, throw, catch, hit, eliminated) with blending
  and a small state machine driven by sim tags and events; clip timings are tuned to the
  ability phase durations after M3, when those stop moving.
- **Dear ImGui** dev overlay: net graph, rollback depth histogram, entity inspector, content reload.
- `RenderState` is derived every frame from two sim states (prev, curr), an interpolation alpha,
  smoothing offsets and the event buffer. Presentation never mutates `WorldState`.
- UI (menus, lobby, scoreboard, settings) is separate from gameplay rendering.
- Local multiplayer is first-class: several humans on one client each own a player slot, all
  predicted locally; the server sees them as separate players behind one connection.
- M2 client flow (v0.15, `apps/client`, the headless-testable `kfm_client_core` under the SDL
  and ImGui glue): the lobby is client-side, before the world exists, because `MatchConfig`
  fixes the present mask at `initialWorld`. Slot 0 is bound to keyboard profile 0 at startup;
  an unbound device joins the lowest free slot by pressing Throw or Dash (any button on a pad);
  bots (scripted inputs from an `Rng` separate from the world's, seeded by `--players-seed`,
  default the same as `kfm_replay --input-seed`) fill the lowest free slots after the humans;
  Enter, pad Start, the Start button or `--autostart` creates the world. After `MatchEnded`,
  Enter or Start returns to the lobby with bindings kept. A removed pad's slot repeats its last
  full input (held buttons cannot re-fire thanks to edge detection) until the next free device
  presses a button (that press is used by the takeover and does not throw or dash), and
  becomes a bot on restart. A Throw or Dash held from the lobby does not fire when the match
  starts. Restart "same seed" reproduces the run
  exactly. Live content editing from the F1 overlay (dash phases and window, banana flight,
  player speed, hit-stun, mode) is a local-only debug feature: the overlay shows the content
  hash, marks it TUNED when it differs from the defaults, and prints the values as C++ so a
  tuned result is pasted back into the `constexpr` tables and the corpus regenerated. The HUD
  (scores, round, countdown, banners, `P<n>`/`BOT<n>` tags with ammo pips placed with
  `worldToScreen`) is drawn on ImGui's background draw list, over the scene and under ImGui
  windows. Per mode (v0.23, from `RenderHud`, which the builder fills from `MatchRules` and the
  `ModeDef`): rounds shows round-win pips and "Round N (first to K)"; survival the same plus the
  alive count and the round clock; deathmatch each slot's eliminations (dimmed with a countdown
  while it waits to respawn), "first to K" and the match clock. The clock is `m:ss`, pops on
  `TimerWarning` and turns red in the last 10 s; a drawn match shows "Draw!". The browser page turns its query string into the same flags
  (`?autostart&bots=3&seed=7&overlay`).
- M4 lobby (v0.24, `apps/client/core/lobby_menu.{hpp,cpp}`, `lobby_settings.{hpp,cpp}`, drawn
  by `hud.cpp`). The lobby window shows a row of ten slot cards (the slot's palette colour as a
  stripe, `P<n>`/`BOT<n>`, the device: keys left/right, pad #id, bot, or free and dimmed) and,
  locally, the settings panel beside an arena preview. `LobbyMenu` is a headless state machine
  over rows built from the content: Arena, Mode, one toggle per `PowerUpDef`, Bots (0–9, then
  clamped to the free slots), Volume (steps of 10 %), Sound (mute). Up/Down move the focus
  (wrapping), Left/Right change the focused value (arena and mode cycle with wrap-around, a
  power-up toggles, numbers clamp); the mouse focuses a row and has the same arrows; Enter/Start
  still starts. `MenuInput` turns raw `InputEvent`s into those four actions: arrows and WASD on
  the keyboard, a pad's d-pad and left stick (fires once past 0.6, re-arms under 0.35, per pad),
  and only for pads already bound to a slot, because an unbound pad's press is its join. It never
  reads game bindings, so the join keys (Throw/Dash) and a future third ability button stay
  free. A change applies at once to the lobby's `MatchSession` (`setArena`, `setMode`,
  `setPowerUps`, `setBotCount`) and to `ClientAudio`; the -/= keys, joins taking bot slots and
  the F1 audio panel flow back into the panel. The preview draws the arena def top-down (floor,
  walls, pits with their rim, slow patches, spawn points in slot colours, power-up spawners
  filled when the selected set leaves them something to spawn, hollow when it switches them
  off) next to the mode's key numbers (`modeSummary`: "first to 10 eliminations, 3:00, respawn
  2.0 s (1.5 s protected)") and the power-up set. Persistence: the lobby writes the settings
  back into `ClientOptions` as names (`--arena`, `--mode`, `--powerups`, `--bots`, `--volume`,
  `--mute`) and saves them as `key=value` lines (`settings.txt` in `SDL_GetPrefPath("kfm",
  "kung-fu-monkeys")` natively, `localStorage['kfm.settings']` in the browser). Startup applies
  defaults, then the saved text (a name the content no longer has, a bad number or an unknown
  key is skipped with a warning, never fatal), then the command line again, so a flag or URL
  parameter (`?arena=pillars&mode=deathmatch&powerups=shield,big_banana`) wins for the run;
  `--no-settings` neither reads nor writes (the smoke tests use it). In network mode the local
  lobby hides the panel: the server owns the match. First browser run (headless Chromium, the
  wasm build): `?arena=pillars&powerups=shield,big_banana` showed in the panel, arrow and WASD
  presses changed the mode and a power-up, `localStorage['kfm.settings']` held the new set, a
  reload without parameters restored it, `?no-settings` ignored it; no console errors.
- HUD layout (v0.24): the scoreboard is one card per present slot along the top, the lobby's
  card style, sharing one width so the row never jitters and shrinking to fit narrow windows:
  the label and the score on the first line (round-win pips in rounds and survival,
  eliminations with the respawn countdown in deathmatch), the slot's live power-up timers on the
  second (a pie per power-up in its colour with whole seconds, only when someone has one); a
  card dims while its slot waits to respawn or is out of the round. The mode line and clock sit
  under the cards. Over each monkey the same pies, smaller and without numbers, follow the ammo
  pips.
- Teams in the client (v0.32): a team is drawn in the palette colour of its index (team 1 red,
  team 2 blue), named "Team N". The lobby gains a read-only Teams row after Mode ("free-for-all"
  or "2 (by mode)"; Left/Right do nothing), the Arena row skips arenas the mode does not suit
  and a mode change moves to the first that does; in a team mode each filled slot card carries
  "T<n>" and a bar in its team colour (`MatchSession::teams()`: the balanced split of the
  joined slots and bots, the match's `MatchConfig::teams` once it runs), and the preview says
  "Teams 2, balanced as players join". In the match `RenderHud` carries `teamCount`, every
  slot's team and every team's score; the scoreboard cards get the same team bar, a line of
  team scores in team colours sits under the mode line, and the round and match banners say
  "Round won by Team N" / "Team N wins the match!". `?mode=team_rounds` needs nothing new: it
  maps onto `--mode`, which takes the first compatible arena.
- Objectives in the client (v0.33): `RenderState::zones` (`buildZones`) holds each zone as a
  floor rectangle, nobody's (white) unless the match is `capture`, where zone t is team t's and
  takes its colour; the renderer draws it first in the decal pass as a `DecalMode::Rect` on a
  unit quad mesh (`kQuadMesh`, `makeFloorQuad(0.5, 0.5)`), `kZoneOpacity` inside and
  `kZoneRimOpacity` on the band from `kZoneRimStart` out. A flag pickup is `PickupKindView::Flag`
  drawn by the power-up path in its team's colour with `kFlagShape` (diamond) and the pulsing
  ring; taking one sounds and bursts like a power-up. `PlayerFlag::Carrying` (the enum widened
  to u16) puts a small spinning gold diamond over the carrier. The HUD's mode line names the
  kind ("Hill (first team to 5 points)", "Capture (first team to 3 flags)"), the start banner
  says "King of the hill" / "Capture the flag", and the cards count points like deathmatch
  (`sim::scoresPoints`); the team score line is step 2's. The lobby's arena preview draws the
  zones (translucent, lettered); the hill holder is not drawn yet (`HillHolderChanged` is
  there for it).

- The front end (v0.43, M6b step 3; `apps/client/ui_style.{hpp,cpp}`, `screens.{hpp,cpp}`,
  `hud.cpp`, the headless models `core/menus.hpp`, `core/online_menu.hpp`,
  `core/scoreboard.hpp`, and since the architecture audit `core/hud_view.hpp`: `HudView`, the
  winner's title and colour, the clock's urgent threshold, the match-over screen's typed
  `MatchOverAction` buttons and `roomSlots()`, what each slot of the waiting room shows, with
  the bots to come taken from the server's placement in `RoomState`, v0.75, and the slots at
  or above the server's player cap `Closed` and given no card, v0.76). Dear ImGui stays the drawing library. Font: Lilita One (OFL 1.1,
  `assets/fonts/` with its licence), compiled in by `cmake/EmbedFile.cmake`, loaded once by
  `ui::applyUiStyle()` as ImGui's default font at 20 px; one `ImFont` draws every size (ImGui
  1.92+ bakes glyphs per size), and the F1 overlay pushes ImGui's own font at 13 px. Palette
  (`ui::kBanana`, `kCream`, `kJungle`, `kLeaf`, `kBark`, ...) shared by the theme and the HUD;
  slot and team colours stay `presentation::playerColor`. Screens: the main menu (title, Play
  online, Play local, Settings, Quit natively, the focused item's hint, the protocol and content
  hash in the corner), Settings (name, volume, sound, bot difficulty, Back), Play online (region
  with availability, Create room, room code and Join, Back), the local lobby (cards, compact
  settings rows with the power-up toggles two to a line, preview, Start), the network status
  (connecting, the room code with Copy link / Share, refusals in plain words), the room's waiting
  screen and the match-over screen; the scoreboard between rounds is drawn on the HUD's
  background list. Navigation: the models take `MenuAction`s from `MenuInput` (every device on a
  menu screen, the bound ones in the local lobby), `isMenuActivate()` (Enter, keypad Enter,
  Space, a pad's South or Start) and `isMenuBack()` (a pad's East or Back; Escape is the app
  shortcut); hovering with a moving mouse takes the focus, a click acts. Flow: menu → Play local
  → lobby → match → match-over → Play again or lobby; menu → Play online → a room's status →
  waiting → match → match-over → Play again (host) → waiting; Escape steps back towards the
  menu, and the menu quits natively. v0.46: over a running match Escape (or a bound pad's Start)
  opens the pause menu instead (`PauseMenu`: Resume, Back to lobby, Main menu locally, the match
  paused; Resume, Leave room online, the match running on with the local slots idle); Escape, a
  pad's East, Back or Start resumes; P and O stay the developer's pause and single step. `--autostart` and the network flags skip the menu as before;
  `--screen` opens a given screen.
- M3 network mode (v0.17, `apps/client/core/net_session.{hpp,cpp}`): a separate `NetSession`
  beside `MatchSession`, so the local path (bots, pause, step, time scale, content tuning,
  `FixedStepClock`) is untouched; the two meet only through the lobby's `InputSampler`.
  `--connect ws://host:port [--token T] [--spectate] [--stun URL] [--players N]` (the browser
  takes the same names in the query string) creates a `WebRtcTransport` client and a
  `NetClient`; the host sequence is `beginFrame`, `pump`, `reconcile`, `onRollback` if needed,
  one `tick` per `ticksDue` with inputs sampled over that tick's wall-clock window (the batch
  ends at the frame's time and earlier ticks count back at the 98–102 % rate the batch ran at,
  `NetClient::tickStartNs(i)`), then `drainEvents` into the frame events. The lobby's bound
  devices in slot order become devices 0, 1, …; their count is the requested slot count
  (`--players` overrides, `--spectate` requests 0); when the session reaches Running, device i is
  bound to the i-th set bit of `localSlots()` and held buttons are consumed. A slot without a
  device repeats its last input and can be taken over by a free device's press as in M2.
  Status flow: Connecting → Syncing → Running, or Rejected / Disconnected / Failed with a reason
  on a status window that replaces the lobby; Escape disconnects and returns to the local lobby,
  whose Start then reconnects (v0.43: Escape returns to the main menu, whose Play online
  reconnects to a `--connect` server when no lobby is configured; v0.46: during a running match
  Escape opens the pause menu, whose Leave room disconnects). Online, pause, step, time scale, bot keys, restart and content
  tuning do nothing (the server's clock and content rule; a content edit would break the hash)
  and the overlay says so. The F1 overlay gains the §7 net panel: RTT/jitter/loss with a graph,
  lead against target, arrival margin, clock rate, rollback depth graph and histogram,
  mispredictions and substitutions per second, journal counts, checksums compared/mismatched,
  resyncs, bytes/s each way. Remote humans and server bots both read `P<n>` (the roster carries
  no bot flag). First end-to-end runs: a native client and the browser page each joined a real
  `NetServer` over WebRTC on one machine (RTT about 3 ms in the browser, roughly 60 rollbacks/s
  of depth ≤ 3 against bots, 0 checksum mismatches); the 3 s real-socket client test
  (`kfm_client_net_tests`, label `network`) syncs in about 185 ms.
- Replay viewer (v0.68, M7 step 6; `apps/client/core/replay_session.{hpp,cpp}`,
  `core/replay_controls.{hpp,cpp}`, `Screen::Replay` in `main.cpp`, drawn by `hud.cpp` and
  `screens.cpp`). Entry: `kfm_client --replay SRC` (a path natively, a URL in the browser, where
  `?replay=<url>` becomes the flag through `shell.html`'s generic query mapping, beside
  `config.js`'s `--lobby`) opens the viewer directly; it excludes `--autostart`, `--connect`,
  `--room`, `--create` and `--screen`, and there is no main-menu entry. Loading: natively
  `readFileBytes` before the first frame; in the browser an `EM_JS` `fetch(url, {mode: 'cors',
  credentials: 'omit'})` with an `AbortController`, polled once per frame (nothing blocks, no
  exported functions or link options), the bytes copied into a buffer C++ allocated (both behind
  `IPlatformServices::startDownload` / `pollDownload`, §13.4); both cap
  the file at `kReplayMaxBytes` (16 MiB, in `replay_session.hpp`: a larger Content-Length is
  refused up front and the stream reader cancels past the cap; `readFileBytes` refuses a larger
  file from its size before reading and reads straight into the `std::byte` vector), and leaving
  the screen aborts the fetch. The ladder's bucket allows GET from `https://kfm.minip8.com`,
  which is what lets the page read it (the site sets no COOP/COEP). `ReplaySession` (SDL-free,
  in `kfm_client_core`): `load()` decodes the header and every `InputFrame` into a vector up
  front and refuses a bad magic, version or short header, truncated or out-of-order frames, a
  `tickCount` the remaining bits cannot hold (refused `BadFrames` before anything is allocated,
  "the replay is truncated: it claims N ticks but holds at most M": the smallest encoded frame
  is 48 bits, a `u32` tick and a `u16` present mask, so at most `bytes.size() / 6` frames are
  reserved), out-of-range arena, mode or loadout indices, zero ticks, and a content hash other
  than the client's `sim::contentHash(content)` (the message names both: the viewer is the
  client of the replay's own release, v0.59); the build hash is shown, not checked (every build
  writes 0). Playback exposes the render surface of `MatchSession` and `NetSession` (prev/curr,
  `renderPrev`, `renderAlpha`, `frameEvents`, the presentation state and its clock), so the
  builder, the renderer and the fixed camera are a match's. Controls: play/pause, single step,
  speeds `kReplaySpeeds` {0.25, 0.5, 1, 2, 4} (the clock's time scale;
  `FixedStepClock::kMaxTimeScale` is 4), restart and `seek(tick)`: forward steps from keyframes:
  one every `kReplayKeyframeTicks` = 600 ticks (10 s), each a `WorldState` copy of 6096 bytes,
  recorded the first time playback or a seek steps onto that tick, kept across `restart()` and
  cleared on `load()` (a 16 MiB replay, ~300 k ticks, holds at most ~500, ~3 MB; a decoded frame
  costs 56 bytes). A seek starts from the newest keyframe at or before its target when the
  target is behind the current tick or that keyframe is ahead of it; otherwise it steps on from
  the current tick, or from tick 0 when no keyframe exists. So once a span has been played or
  sought through, any seek into it steps `target % 600` ticks (fewer than 600); the first visit
  of a span costs its length (~45 ms for a whole 10-minute replay at the release bench's ~820 k
  ticks/s). `lastSeekSteps()` and `keyframeCount()` expose this to tests. A seek emits no
  events, resets the presentation state and draws a still frame. Interpolation: below 1x the
  viewer always interpolates (at 0.25x on a 60 Hz display it would otherwise hold each tick for
  four frames); at 1x and above it follows `MatchSession`'s rule (the newest tick when the
  display rate is at least 60 × speed × 0.95). While still (just loaded or sought), at the end,
  or paused (after a single step too) it draws the newest state alone (alpha 1, `renderPrev()`
  is `curr()`), whatever the policy. Up to `kReplayMaxTicksPerFrame` = 8 ticks run per frame (4x
  on a 30 Hz display), `kMaxFrameEvents` sized to match; at the end the last state is held. No
  bot runtime: a replay is inputs. The screen: "Loading replay…" with a spinner and the source
  over a plain background; on failure "Could not open the replay", the reason, the source and a
  Main menu button (Enter, Escape and a pad's buttons also go to the menu), and `error: replay
  SRC: …` on stderr. During playback the HUD is a spectator's: slots "P1" to "P10" in team
  colours, no "you" marker, no aim bar (replays carry no names); the between-rounds scoreboard
  moves up and shrinks (to 0.6x) to stay clear of the bar (`HudView::bottomInset`). The control
  bar along the bottom (ImGui, the `ui_style` palette, `ui::bigButton` / `showsFocus` /
  `pointerTakesFocus`): the title "Replay: <arena>, <mode>, N players"; Restart, Play/Pause,
  Step (only while paused and before the end), the speed button ("1x"; a click cycles and
  wraps), the timeline (a click or drag seeks only when the tick under the pointer changes, and
  playback holds while the knob is held), "m:ss / m:ss", Main menu. The layout is the headless
  `replayBarRows(ReplayBarPieces)`: one row when the one-row timeline (the bar's content width
  less the transport buttons, the time label, Main menu and three gaps) is at least
  `kReplayMinTimelineWidth` = 240 px, else the timeline takes its own row; in the game font that
  is one row from about 987 px of display (two rows at 800 and 852 px, one at 1024 and 1280).
  Keys: Space play/pause, `.` one tick (pausing first), Left/A and Right/D seek 5 s (Shift: 30
  s, clamped), `[`/`]` speed, Home restart, Escape main menu, Up/W, Down/S and Tab move the
  focus, Enter activates; pads: South activates, East/Back main menu, Start play/pause, West
  step, d-pad Left/Right seek, Up/Down focus, the shoulders speed. The SDL glue drops key
  repeats, so a held key seeks once; shortcuts act at once and the v0.55 highlight rule applies
  to the focus keys. `ReplayControls::refocus(status)` hands the focus to Play/Pause when the
  focused control turns disabled (Step while playing or at the end), before the bar is drawn and
  before a key is applied, and hovering a disabled control does not take the focus.
  `ReplayControls` holds the focus, the key and pad mapping and the time label, tested
  headlessly. At the end the last frame stays with the HUD's final banner and scoreboard (hint
  "End of the replay", no match-over buttons); Play becomes "Watch again" (a restart) and Step
  is disabled. No pause menu, bots or tuning, and no hot reload inside the viewer: with
  `--replay` and `--content DIR` the watcher starts when the viewer leaves for the menu
  (`startHotReload` in `main.cpp`), as it would at startup without `--replay`. F1 opens a
  read-only overlay with a Replay panel (tick and total, checksum, phase, round, paused, speed,
  seed, players, arena/mode/loadout, power-ups, the content and build hashes). Leaving drops the
  replay; nothing in the menu returns to it. At load the client prints `kfm_client: replay SRC:
  1800 ticks (0:30), arena courtyard_hill, mode contest, loadout base, 10 players` (v0.69;
  `courtyard` before). Exit codes: with `--exit-after-frames` a playing replay exits 0 ("stopped
  after N frames in the replay at tick T of C, checksum 0x…") and a failed one renders the error
  screen for the N frames (so `--screenshot` captures it) and exits 1; without it, quitting
  exits 0. Follow-ups are in §12.
M2 presentation decisions (v0.15), all constants in `kfm/presentation/tuning.hpp`:
- World basis: sim `(x, y)` at cosmetic height `h` is world `(x, h, −y)`, right-handed, +y up.
  The camera sits on world +z (the sim −y side), so with an ordinary right-handed `lookAt` and
  `perspective`, sim +x is screen right, sim +y is screen up (away from the camera) and sim
  angles turn counter-clockwise on screen. A sim `Angle` θ is a world yaw of θ; models face
  model +x. Matrices are column-major `float[16]`, uploaded to GL without transposition.
- Camera: vertical FOV 30°, pitch 55° below the horizon, perspective. The distance is solved
  (bisection) so the arena box plus 1.8 units of height above it fills the viewport with a 6%
  margin on the limiting axis at any aspect ratio, and a lens shift (off-axis projection)
  re-centres it vertically. `worldToScreen` serves HUD labels and `screenToGround` mouse aim.
- Height curves: dash hop `0.35·sin(πu)` over the Dash active phase; hit-stun lift
  `0.4·sin(πu)`; a flying banana rides at hand height 0.6 plus an arc of 0.5 that rises as a
  sine over the outbound phase and falls with the remaining distance to the owner while
  returning, so it is on the ground at the throw and at the owner; pickups hover at 0.15 ± 0.08
  and spin slowly. The monkey placeholder capsule is 3.2 radii tall.
- Facing: body yaw follows the dash direction while dashing, otherwise the move direction
  (keeping the last non-zero one), limited to 720°/s; aim yaw follows the aim directly. A dash
  stretches the forward axis by 0.18 and hit-stun squashes height by up to 0.2.
- Effects from events: dash ring 0.25 s, perfect-catch flash 0.4 s, auto-catch flash 0.2 s, hit
  flash 0.2 s, throw puff 0.25 s, elimination burst 0.6 s, round-start / round-end / match-end
  banners 1 / 1.5 / 3 s; at most 64 live effects (128 since v0.19, "Effects" below), the one nearest expiry is replaced when full.
  Screen shake adds 0.12 per hit, 0.3 per elimination, 0.08 per perfect catch, capped at 0.5,
  decays with τ = 0.15 s and oscillates at 23 Hz.
- Palette (slot order): red, blue, purple, green, orange, yellow, cyan, pink, white, charcoal
  (v0.49: yellow and purple swapped, so slots 0–4 never wear banana yellow); every pair at least
  0.25 apart in RGB.
- Placeholder meshes are generated on the CPU (`meshes.hpp`): capsule (16 × 4 per hemisphere),
  banana (a tapered tube bent along a quarter arc whose chord equals the hitbox diameter), disc,
  floor quad whose `uv` is the sim-plane position (a one-unit grid is `fract(uv)`), unit cube.
- Rendering backends in M2: sokol_gfx GL only (`SOKOL_GLCORE` natively, `SOKOL_GLES3` /
  WebGL2 in the browser) with hand-written GLSL emitted for `#version 330 core` or
  `#version 300 es` by a compile-time prefix; no sokol-shdc until D3D11/Metal/WebGPU are
  wanted. Natively GL is loaded through a vendored glad2 loader (`third_party/glad`, GL 4.3
  core, generated reproducibly; the command is in its README) with `SOKOL_EXTERNAL_GL_LOADER`
  and `SDL_GL_GetProcAddress`, so no machine needs system GL headers. The context is 4.3 core
  forward-compatible (4.1 on macOS): sokol decides at compile time from the header whether to
  use texture views and at run time from the driver, so a 4.1 header on a 4.3+ driver renders
  every texture black; 4.3 matches sokol's own Win32 loader. sokol is its own CMake target
  (`kfm::sokol`: one implementation TU, backend define as a public compile definition) so the
  renderer in `presentation` can use it without depending on `platform`.
- Renderer (`kfm_renderer`): two programs, `lit` (opaque meshes and the floor) and `decal`
  (unlit discs for shadows, rings and bursts); v0.49 adds `outline` and `skinned outline` (ink
  hulls, "Look" above) and gives `lit` a sixth fragment vec4. Uniform blocks are flat `vec4` arrays in std140
  layout (matrices as four columns; the C++ structs are built only from `Mat4`/`Vec4` and
  static-asserted to 16·N bytes, so there is no vec3 padding to get wrong); at most 8 vec4s per
  stage, far below WebGL2 limits. One directional light with a three-band toon ramp (two cel
  bands since v0.49); a player
  flash mixes toward white, dashing adds emissive, invulnerability blinks. The floor grid is
  `fract(uv)` with `fwidth` anti-aliasing. Blob shadows are alpha-blended discs just above the
  ground with depth test on, depth write off and a depth bias, darker at the rim so the hitbox
  edge reads; a gold ring around the feet shows the `PerfectDash` window, a stretched trail disc
  shows a dash, a small banana in hand shows ammo, flying bananas are tinted toward the owner's
  colour. Draw order: opaque, alpha ground decals and billboards, additive billboards; nothing is
  sorted. The per-frame path allocates nothing. Measured 47–55 draw calls and under 5k triangles
  for 10 players; budget 100 draw calls per frame before instancing is worth doing. v0.49: an
  outline is one more draw per outlined object (static hulls queue and draw in one pipeline run,
  at most 160 a frame; a skinned body's right after it); `kfm_render_probe` measured 37 draw
  calls on `pillars` with one player, 97 on `island` in a 4-player round with effects, and 145 in
  a 10-player `grid_rounds` on `courtyard` (a tilemap of many wall boxes, each drawn twice): over
  the soft budget, cheap for WebGL2, and the follow-up is to bake an arena's static walls and
  their hulls into one mesh each when the arena loads.
- GPU skinning (v0.15): the one rigged test model of the M2 exit criterion is generated at
  build time by `tools/assets/gen_test_rig.py` (stdlib Python, byte-identical output; CMake
  target `kfm_test_rig`), copied next to each executable natively and embedded in the browser
  build. It is the M2 stand-in for the Blender → glTF pipeline above, which arrives with real
  art; no exported glTF is committed. The rig has 7 joints (root, spine, head, upper and lower
  arms) and six clips (idle and run loop; dash, throw, hit and eliminated hold their last
  frame). cgltf loads meshes, skins and LINEAR/STEP rotation and translation channels into a
  `SkinnedModel` of plain vectors (`kfm::presentation_gltf`, skipped in headless builds); the
  animation layer (headless, tested) samples clips, blends poses (lerp translations, nlerp
  rotations) and produces skin matrices. Clip priority: eliminated > hit (`HitStun`) > dash
  (`Dashing`) > throw (`Throwing`, held at least 0.3 s) > run (ground speed above 0.5 units/s,
  ignoring snap-sized jumps) > idle; any change crossfades over 0.1 s. Per-slot animation state
  lives in the renderer, never in `RenderState`. Budget: at most 16 joints and 4 influences per
  vertex, skin matrices as 72 vec4s (WebGL2 guarantees 256); raise it only with dual quaternions
  or a joint texture. A missing or invalid model, or `--capsules`, falls back to the capsule
  with a warning. Not yet done: the upper-body/lower-body split (body yaw drives the whole
  model; the aim bar shows the aim) and a slumping body after elimination (the sim despawns an
  eliminated player in the same tick, so a presentation-side ghost is needed).
- `kfm_render_probe` (`apps/render-probe`) renders a scripted match through the same builder and
  renderer with `--screenshot`, `--exit-after-frames`, `--exit-on <event>` and `--hide-ui`, so
  rendering is verified from screenshots without a human; it is the second xvfb smoke test on
  Linux CI and builds for the browser too.

Effects (v0.19, M4; `event_fx.{hpp,cpp}`, constants in `tuning.hpp`). Every effect lives in the
`PresentationState` effect list (at most 128, the one nearest expiry replaced when full), is
copied into `RenderState::effects` with its remaining and total seconds, and is drawn by
`kfm_renderer` from the disc mesh as ground decals or camera-facing billboards (alpha or
additive); no shader changed. Event-driven (`applyEventFx`, one call per new `SimEvent`):

| Event | Effects | Sound |
|---|---|---|
| `PlayerDashed` | DashRing 0.25 s, DashAnticipation 0.12 s (a ring collapsing onto the dasher and a flash ahead along the dash, §12) | DashSwish |
| `BoomerangThrown` | ThrowPuff 0.25 s | ThrowWhoosh |
| `BoomerangCaught` Perfect | PerfectCatchFlash 0.4 s, PerfectCatchBurst 0.45 s (8 gold sparks), flash, shake | CatchPerfect (two-note chime) |
| `BoomerangCaught` Auto | AutoCatchFlash 0.2 s, flash | CatchAuto (tick) |
| `PlayerHit`, `PlayerDashHit` | HitFlash 0.2 s, HitSpark 0.25 s (6 sparks in a cone away from the banana or attacker), flash, shake | Hit (thud) |
| `PlayerEliminated` | EliminationBurst 0.6 s, EliminationPoof 0.7 s (5 smoke puffs), shake; v0.57: at the end of a pit fall both rise from the floor where the body went under (its last drawn position at `kEffectGroundHeight`), not at body height over the edge | Elimination |
| `PlayerFell` (v0.57) | PitFall 0.45 s: two rings in the slot colour closing in where the monkey went down; the fall itself is drawn from state (below) | Fall (a slide whistle falling from 1.4 kHz to under 300 Hz) |
| `BananaDropped` (v0.57) | GroundDust 0.35 s where the pickup lies, the grounding look; a stacked pickup (`Pickup::count` > 1) draws up to three bananas (`kPickupStackLift`, `kPickupStackTurn`) | BananaGround |
| `BoomerangFell` (v0.57) | a small PitFall (`RenderEffect::size` = `kBananaSplashSize`) where the banana drops in; its sinking is drawn from state (below) | BananaPlop (a short falling bloop) |
| `PickupTaken` | PickupSparkle 0.35 s at the pickup; a power-up (v0.20) adds PowerUpBurst 0.5 s (a ring and 8 sparks in the taker's colour) | Pickup; PowerUp (rising arpeggio) for a power-up |
| `ShieldBroken` (v0.20) | ShieldShatter 0.4 s (a blue ring, flash and 7 shards), flash, shake | ShieldBreak (glassy crack) |
| `BoomerangGrounded` | GroundDust 0.35 s where it landed | BananaGround |
| `BoomerangBounced` | BounceSpark 0.15 s | WallBounce |
| `RoundStarted`, `RoundEnded`, `MatchEnded` | banners 1 / 1.5 / 3 s; the HUD text pops in at 1.35× and settles over the first quarter (`bannerScale`) | — , RoundEnd, MatchEnd |
| `PlayerRespawned` (v0.23) | RespawnBurst 0.6 s at the spawn point (a ring collapsing in the slot colour and a rising glow); the spawn protection then blinks the body (the `Invulnerable` flag) | Respawn (rising shimmer) |
| `ScoreChanged` (v0.23) | ScorePop 0.5 s (gold ring at the scorer); v0.57: none for a `Suicide` point | Score (coin blip); none for a `Suicide` point |
| `BananaRegrown` (v0.57) | PickupSparkle 0.5 s in the hand (`kRegrowSparkleSeconds`), a 0.15 s flash | Regrow (a soft pop gliding up an octave, then a twinkle) |
| `BananaVanished` (v0.57) | a small white puff (ThrowPuff, `kBananaVanishSeconds` 0.3 s) where the banana was | — |
| `TimerWarning` (v0.23) | TimerBanner 0.8 s: the HUD clock pops | TimerWarning (beep) |
| `PlayerBlinked` (v0.21) | BlinkFlash 0.3 s at the origin (from the payload) and at the landing, in the slot colour; the jump exceeds `kSnapDistance`, so the body snaps | Blink (rising zap) |
| `SlamLanded` (v0.21) | SlamShockwave 0.45 s (a ring racing out to the reach, carried in `RenderEffect::size`), GroundDust, shake | Slam (deep boom) |
| `PlayerSlammed` (v0.21) | GroundDust under the pushed monkey, flash | — (the slam sounds once) |
| `PlayerParryStarted` (v0.21) | ParryGuard 0.2 s (a pale ring flaring at the parrier) | — |
| `BoomerangParried` (v0.21) | ParrySpark 0.35 s (a white flash and 8 sparks at the banana), flash, shake | Parry (metallic clang) |
| `MonkeyCalled` (v0.35) | CallShout 0.45 s over the caller's head (`kCallShoutHeight`): a ring in the slot colour, its size growing with the symbol (`RenderEffect::size` = symbol / 255), and a white burst | CallPing (two bell notes), only for a caller in `PresentationState::pingSlots` (the app's local humans: a human on the ability button hears its own call; bots' calls are silent); the F1 overlay's Calls section lists the match's last eight (tick, slot, symbol). No fog or cone is drawn: humans see everything (§8) |

Sampled from state, not events: a player whose Dash is active leaves a DashTrail afterimage
(0.18 s) every 25 ms, a flying banana a BananaTrail puff (0.12 s, tinted toward the owner) every
30 ms; the count-in sound plays on entering `RoundStarting` and at every whole second the
countdown crosses, RoundGo on `RoundStarting` → `Playing`. Entity positions for events come from
`curr`, else `prev` (despawned this tick), else the entity's presentation track. Player colours
come from `palette.hpp`. Bursts cost one draw call per spark; a busy 10-player frame measured
72–90 draw calls.

Pit falls (v0.57), sampled from state like the trails: a monkey with `Tag::Falling` is drawn
from its fall's progress t = (`PlayerDef::fallTicks` − `Player::fallTicks`) / `fallTicks`,
interpolated between the two rendered ticks like the position (`RenderPlayer::fall`,
`PlayerFlag::Falling`). Over the first `kFallTeeterFraction` (0.18) it hops `kFallHopHeight`;
then it drops `kFallDepth` (2.2) × u² below the floor, and the ground pass, which writes depth
at height 0, hides whatever is under it, so it reads as sinking into water, mist or sea on every
theme without a hole in the mesh. It sinks where it stands: the sim starts a fall only with the
whole body over the pit, so nothing sinks through solid ground (the first cut of v0.57 fell on
the centre and slid the drawn body into the pit; that tuck is gone). It shrinks to `kFallEndScale`
(0.5), tips its nose down by `kFallTilt` (0.7 rad, `RenderPlayer::tilt`, a rotation about the
model's side axis), spins `kFallSpinTurns` (0.75) turns as t², darkens toward `kFallShade` by
`kFallDarken` (0.55) × t, and plays the `hit` clip; its shadow shrinks and fades with 1 − t;
its HUD tag and aim bar are not drawn, but a banana in its hand is, shrinking with the hand
as it goes down. A banana dropped at a death comes down with the grounding look every grounded
banana has (GroundDust, BananaGround) and is drawn as the pickup; one that came down over a pit
(`Sinking`) sinks from hand height to `kBananaSinkDepth` below the floor as t² over
`pitSinkTicks`, its shadow shrinking away; the floor's depth hides it as it does the monkey. All numbers live in `tuning.hpp`. The body
is gone well before the sim's last falling tick, and the elimination's burst then rises from the
pit. `kfm_render_probe --exit-on pit-fall|fall [--fall-at PERCENT]` stops on a fall's first
frame or once a fall is that far through (the screenshots for this change).

Rollback rule: effects and sounds are started only from the event stream the builder is given,
each event once: locally the per-tick sink, online `EventJournal::drain()` (§5.9). A
re-simulation therefore never re-triggers an effect or a sound, an event retracted before it was
drained spawns neither, and one already played stays played (it is counted as retracted).
State-sampled trails follow whatever timeline is drawn.

Power-ups (v0.20, §5.8). A power-up pickup (`RenderPickup::kind == PowerUp`, with the def's
`colorIndex`, `shape` and index) is drawn from the placeholder meshes in its palette colour,
spinning 0.45 units up with a slight glow: `Orb` a capsule squashed to a ball, `Cube` a tilted
cube, `Diamond` a cube on its corner, `Coin` a flat capsule on its edge (`kPowerUpSize` 0.6), over
a pulsing ground ring in the same colour; a grounded banana looks as before. `RenderPlayer`
carries its live power-ups (`powerUps[0..powerUpCount)`: index, colour, shape, seconds left,
fraction left) and the HUD draws them under the ammo pips as small pies in the power-up's colour
with whole seconds beside them. A shielded player keeps the blue ground ring of `Shielded`. The
F1 overlay lists each player's power-ups and each pickup's def.

Audio (v0.19, M4). Everything SDL-free is in `presentation` and headless-tested; only the device
is in `platform`.
- `SoundBank` (`sound_bank.hpp`): 14 clips (`SoundId`: throw, dash, perfect and auto catch, hit,
  elimination, pickup, banana landing, wall bounce, count-in, go, round end, match end, UI click;
  v0.20 adds power-up and shield-break, v0.23 respawn, score and timer warning, 19 in all)
  synthesised once at startup from sines, inharmonic bell partials, state-variable-filtered
  xorshift noise and envelopes, mono float at 48 kHz, each normalised to a 0.8 peak with 1 ms /
  4 ms end fades, deterministic (fixed noise seeds), about 4.6 s of audio in total. No audio
  asset ships and the browser download does not grow. Each clip has a mix gain.
- `Mixer` (`mixer.hpp`): 16 voices; `play(id, gain, pan)` takes a free voice or steals the
  oldest; gain = cue × clip × master volume; constant-power pan; muted or volume 0 mixes silence
  while voices keep advancing (unmuting never replays stale sounds); the sum is hard-limited to
  [−1, 1]; `mixInto(float* interleaved, frames)` is the pull API. No allocation after
  construction.
- Cues: the builder writes the frame's `SoundCue`s into `RenderState::sounds` (at most 32) from
  the table above. Positioned sounds pan by world x: 0.6 × x / arena half width. The same sound
  twice in one frame merges into one cue, +25 % gain per repeat up to 1.75× (identical clips
  started on the same sample only double in amplitude).
- `platform::AudioDevice`: the default playback device as an `SDL_AudioStream`, float32 stereo
  at 48 kHz (SDL converts to the device), opened lazily with `SDL_InitSubSystem(SDL_INIT_AUDIO)`
  and attempted once. No driver, no device or any failure leaves it closed and the client silent,
  never an error: CI, xvfb and `--exit-after-frames` runs print one `kfm_client: silent: ...`
  line. There is no audio callback: every frame, on the main thread, `pump()` tops the stream's
  queue up to 50 ms by calling `Mixer::mixInto`, so the client stays single-threaded (SDL's
  device thread natively, or the Web Audio callback in the browser, only drains the queue). The
  queue length is the sound latency and the stall a frame survives without a gap; underruns are
  counted.
- Browser gesture rule (§13): an `AudioContext` may only start after a user gesture, so the
  browser client opens the device on the first key, mouse button, touch or pad button, and SDL3
  resumes its context once `navigator.userActivation.hasBeenActive`. Natively the device opens on
  the first frame with sound enabled. First browser run (`check_browser.py`, headless Chromium):
  no device before input, "audio open: emscripten, 48000 Hz" and a running `AudioContext` after
  the key presses, 60 fps unchanged, no console errors (one Chromium deprecation warning about
  SDL's `ScriptProcessorNode`); with `?mute` no `AudioContext` is created.
- `kfm_client --mute` and `--volume 0..100` (default 80; `?mute&volume=50` in the browser) set
  the mixer; the F1 overlay's Audio panel (local and online) toggles mute, sets the volume, plays
  a test click and shows the device status, voices, steals and underruns. The client glue is
  `apps/client/client_audio.{hpp,cpp}` (`ClientAudio`: `init`, `frame`, `onUserGesture`,
  `click`, `setMuted`, `setVolumePercent`), which the lobby UI uses for its own controls and
  clicks.
- Not yet: music, distance attenuation, per-player local-vs-remote mixing; no event exists yet
  for power-ups (M4 power-ups add theirs to the table). Volume and mute are saved with the lobby
  settings since v0.24 (the lobby panel edits them too).

Character drawing sits behind one interface in `presentation` so that nothing else knows
whether a character is a skinned mesh or a quad. The earlier plan, pre-rendered 8-direction
sprites from the same Blender models, remains the fallback if the 3D renderer stalls M2: the
art is identical up to the export step, and nothing below the presentation layer changes.

---

## 8. Bots (`engine/ai`, the bot API)

Decided (v0.26): bots are a public contract. Anyone writes a bot in any language, the game hosts
it, and matches between bots are reproducible and fair by construction. This replaces the
earlier plan of an in-repo training stack (nanobind env, PPO baseline, ONNX inference), none of
which the contract needs.

- **The contract.** A bot is a WebAssembly module (core wasm, no WASI) behind a small versioned
  C ABI. Exports: `kfm_abi_version() → u32` (the `Observation` schema version the module was
  built against; the host refuses a mismatch), `kfm_obs_buffer() → ptr` (where the host writes
  the observation each tick), `kfm_init(seed: u64, slot: u32)` and
  `kfm_act(tick: u32) → u64` (the packed 5-byte `PlayerInput`, §5.4). Imports: none required;
  an optional `kfm_log(ptr, len)` whose output the host discards or captures in the result.
  No clock, no randomness beyond the seed, no I/O: a module is a pure function of its
  observations, so a match is reproducible from seed, match config, content hash and the bot
  modules. Wasm float arithmetic is specified bit for bit, so trained policies reproduce too.
- **`Observation`.** A versioned, little-endian, fixed-layout POD built in C++ by `engine/ai`
  (`buildObservation(WorldState, MatchContent, PlayerId, delay) → Observation`) and mirrored by
  each SDK. Full information (the game had no fog of war until a mode's `vision` adds one,
  v0.35, below): self (position, velocity, facing, HP,
  ammo, per-slot cooldowns, modifiers, tags), every player (position, velocity, HP, ammo, alive,
  dashing, relation to self), every banana (position, velocity, phase, owner relation, time to
  closest approach), pickups, the arena's statics, the match (mode, tick, scores, round state)
  and the bot's own input delay in ticks. A schema change bumps the version and is a breaking
  change for bots; the v1 field list is pinned when M5 starts.
- **Helpers live in the SDKs, not the host.** Nearest banana, time to impact, raycasts, "can I
  dash-catch now" are SDK functions running inside the bot's own budget. The host ABI stays
  four exports, which is what keeps it stable across SDK growth.
- **`IAgent`.** `struct IAgent { virtual PlayerInput act(const Observation&, Tick) = 0; }`,
  implemented by `WasmAgent` (one module instance per slot, with its own linear memory even
  when two slots run the same module) and by the built-in reference bot in C++. Events reach a
  bot folded into the observation, not through a callback. Agents live outside the sim.
  Server-side bots write straight into the authoritative frame (no network); the native client
  runs bot modules as local players through the same runtime; the browser client instantiates
  them through the browser's own `WebAssembly` API behind a JS shim, with metering best-effort
  there, because contests run on the server.
- **Budget in instructions, never wall clock.** Each bot gets a fuel budget per tick (wasm
  instructions, metered by the runtime). Tournaments run at thousands of times real time, so a
  wall-clock budget would make results machine-dependent. Exceeding the budget or trapping
  costs the bot that tick: the host substitutes "repeat last input", exactly the rule the
  server already applies to a late network input (§6.1), and counts it in the result. Module
  size and linear-memory caps are contest rules; the numbers are pinned with the runtime.
- **Human-likeness is a host setting, not part of the bot.** Bot versus bot: no delay, no noise;
  fuel is the only fairness control. With humans in the match: the host delays a bot's input by
  the humans' lead in ticks, so the bot acts on state as stale as a human's; the observation
  carries that delay so the bot can predict ahead the way a human does. Difficulty tiers
  (reaction delay, aim noise) exist only for the built-in reference bot in casual lobbies and
  never apply to a contest bot.
- **The reference bot.** A utility AI (dodge / approach / throw / dash-catch scorers) in C++
  in `engine/ai`. It fills lobby slots in the shipped game (and yields a slot to a joining
  human, an M3 follow-up) and is the baseline a contest bot must beat; its source is also the
  worked example in each SDK language in the bots repository. The scripted input generator
  (§9) stays as test scaffolding and is not an `IAgent`.
- **The headless match runner.** `kfm_arena` (`apps/arena`): bot modules or built-in bots by
  name, seed, arena, mode and power-up set in; the match played at full speed; a `.kfmr` replay
  (playable by `kfm_replay` and the client) and a result line (placements, scores, per-bot
  fuel use, traps and substitutions) out. It is the contest's local test tool, the ladder's
  engine and, for anyone who wants to train by self-play or search, all the environment the
  project provides.
  As built (v0.30): `kfm_arena [--bot SPEC]... [--bots N] [--seed S] [--matches M] [--ticks N]
  [--arena NAME] [--mode NAME] [--powerups LIST] [--content DIR] [--fuel N] [--delay T]
  [--replay out.kfmr | --replay-dir DIR] [--result FILE] [--json] [--quiet] [--log]`. `--bot`
  fills slots in order and `--bots` pads with `ref:hard` (at least two slots); match k of a
  tournament uses world seed S + k and seeds slot s with the shared formula; `--ticks` caps a
  match (default 36 000, reported as `result cap`, not an error); `--delay` applies to every
  slot; `--log` prints each module's captured `kfm_log` text to stderr. One result line per
  match (`match k seed S ticks N checksum H result ended|cap winner W | slot i SPEC place P
  score S kills K deaths D acts A subs U [traps T over O bad B fuel_mean F fuel_max M fuel_init
  I log L dead no|<init fault>] | ...`) plus a `summary` line (matches, ended, draws, capped,
  mean ticks, per slot wins, mean place, totals); `--json` prints the same as one object per
  line (`--rotate` plays bot b in slot (b + k) mod n in match k, labels a slot several bots
  played `mixed`, and adds per-bot totals: `bot B SPEC wins ...`; every slot entry carries
  `bot B`, and in JSON `"bot"` is that index and `"spec"` the name). Places: the `MatchEnded`
  winner first, then higher final score, fewer deaths, lower slot, all distinct; kills and deaths come from `PlayerEliminated` (a self-elimination is a
  death, not a kill); the final checksum lets a replay be checked against its line. Exit codes:
  0, 2 bad arguments or content, 3 a module refused at load (or a module spec in a build without
  `KFM_WASM_BOTS`), 4 I/O. `kfm_arena_core` (`runMatch`, placements, formatting) is the library
  under test, its agents from the hosts' shared `BotFactory` (below); the CLI is a thin wrapper, and it builds under Emscripten
  as a node CLI with reference bots only. Release, this machine: a 4-bot reference tournament
  runs about 40–50 k ticks/s, dominated by the bot's own evaluation (20 k before the content
  hash was cached), four `echo.wasm` modules about 8× that, and `kfm_replay bench` steps the
  same sim at about 1 M ticks/s.
- **Hosts as built (v0.30).** The server (`apps/server`): `engine/net` has exactly one bot hook,
  `ServerConfig::botInputs` (a function given the world, the content and the tick's frame with
  the bot slots present, called once per tick before assembly, after a restart has been
  applied, so it sees the fresh world and the new `botSeed()`); unset, `NetServer` keeps the
  scripted generator, which is what netsim and the netcode tests still run. `ServerHost` owns an
  `AgentHost` and a `BotFactory`, takes `--bot SPEC` (repeatable), `--bots N` (pads with
  `ref:hard`), `--bot-delay auto|N` and `--fuel N`, re-inits every agent on the first tick of
  each match (bot seed B₀ = `--bots-seed`, Bₖ₊₁ = splitmix64(Bₖ), slot seed from the shared
  formula; a module gets a fresh instance), refuses to start on a module that fails to load,
  and reports per-bot acts, substitutions and, for modules, traps, over-budget ticks and mean
  fuel in its stats line. The "humans' lead" is estimated per Input message as the arrival
  margin plus half the round trip (the lead controller drives the margin itself to +1..+2 ticks
  whatever the RTT, §6.2, so the margin alone is not the lead; the round trip is taken from the
  message's acknowledged frame), smoothed like the margin, and `humanLeadTicks()` is the lower
  median over the active peers that hold slots, clamped to 60, 0 when none, which is why bot
  versus bot has no delay; `auto` re-applies it to every bot once a second. A delay change never
  replays an answer: a longer delay holds the last input until the line catches up, a shorter
  one skips the overdue answers. The client (`apps/client`): `MatchSession` owns an `AgentHost`
  fed by a `BotRoster` that turns one `BotSpec` into fresh agents at every match start and
  restart (delay 0, the shared seed formula, `--players-seed` as the base); the casual default
  tier is `medium`, the lobby panel has a "Bot difficulty" row saved as `bot_difficulty`, the
  flags are `--bot-difficulty easy|medium|hard`, `--bot-module PATH.wasm` and `--bot SPEC`
  (the browser page's `?bot=easy`); a module that fails to load (or any module in the browser,
  which has no runtime yet) falls back to the reference bot at the chosen tier with a logged
  error, so the client always starts; the F1 overlay's Bots section shows per-slot acts and
  substitutions and, for modules, traps, over-budget ticks, bad returns, fuel and a dead flag.
  The scripted generator is used by no shipping host any more: `kfm_replay`, the render probe
  and netsim keep it as test scaffolding.
- **Outside this repository.** A separate bots repository holds the SDKs (one copy-in file per
  language with no dependencies: C, Rust and AssemblyScript to start; v0.36 narrows the ladder
  to C++ over the C header, "The ladder" below), starter and example
  bots, the contest rules and, later, the ladder service. The dividing line: anything that
  links into the game binaries lives here, anything that produces a file the game loads lives
  there. The project ships no training tooling: a trained policy is a weight array in a source
  file plus the forward pass the bot writes. No PyTorch export, no ONNX, no Python environment;
  the nanobind `VecEnv` is dropped, not deferred, unless someone asks for it with a use.
  Seed (v0.30): `tools/bots/sdk-c/kfm_bot.h` is the C SDK the bots repository starts from, a
  single freestanding C11 header that mirrors Observation v1 with static asserts and reproduces
  the engine's sine table, atan2, `Fixed`, `Rng` and one banana flight tick bit for bit (checked
  against the engine by `example_bot_test`); `tools/bots/examples/` holds `idle.c` (the minimal
  skeleton) and `chaser.c` (approach, dodge, dash-catch, throw) with their compiled `.wasm`
  checked in (the compiler is recorded beside them; `build_example.sh` rebuilds them with any
  clang that targets wasm32), and `docs/bot-abi.md` §5 pins the build flags (MVP plus mutable
  globals, no binaryen pass, no libc imports). In the test, two chasers against two medium
  reference bots over four matches win 14 rounds to 0 at about 7 k instructions per act (max
  29 k), with 0 faults, and the match replays to identical checksums.
- **Decided at M5 (v0.30): the runtime, the numbers and the ABI as built.**
  - *Runtime.* WAMR 2.4.5 (`kfm::wamr`, §10), classic interpreter only, built with
    `WAMR_BUILD_INSTRUCTION_METERING`, no AOT/JIT/SIMD/WASI/libc, software bounds checks
    (`WAMR_DISABLE_HW_BOUND_CHECK`: no signal handlers, no guard reservations, ten instances per
    match, identical behaviour everywhere), bulk memory, reference types and the other features
    clang emits for wasm32 by default. wasm3 was passed over: it is in minimal maintenance.
    WAMR has no public getter for the remaining count and its interpreter never writes it back,
    so `third_party/wamr` applies a two-anchor source patch at configure time (the loop's exits
    store the remaining count into the exec env; a fresh exec env starts metered so a `start`
    function cannot hang the host) and a tiny C extension reads it; the patch fails loudly if the
    pin changes. `kfm_ai_wasm` is a separate target so `kfm_ai` never sees WAMR; the option
    `KFM_WASM_BOTS` (default on natively, off under Emscripten) builds without it. A fuel unit is
    one executed opcode plus one per function entry, as WAMR meters it; fuel use is exact and
    deterministic (limit minus remaining), and an over-budget tick uses exactly the limit. WAMR
    meters one unit per opcode regardless of its operands, so the few opcodes whose host work
    grows with an operand would be metering holes: a 16 MiB `memory.fill` (a single `memset`) or
    `memory.copy`/`memory.init` the same, and `table.fill`/`copy`/`init`/`grow` and `memory.grow`
    their counterparts, cost one unit but hundreds of GiB of host work in a tight loop. The same
    configure-time patch therefore charges each extra fuel in proportion to its operand — ⌊len/8⌋
    units for a bulk-memory op (`len` bytes, about a compiled word loop), one per element for a
    table op, 8192 per page for `memory.grow` — as a pure integer function of the operands (so a
    match still reproduces bit for bit), taken before the work so an op that cannot afford it does
    not run and the tick is over budget; the charge sits after the op's bounds check, so an
    out-of-range op still traps. Turning bulk memory and reference types off was the alternative,
    but `docs/bot-abi.md` accepts them and current Rust toolchains emit them, so charging keeps
    existing modules working. Instantiation fills declared data, element and table segments outside
    any instruction budget, so a table is capped at 65 536 elements at load (`maxTableElements`)
    beside the 16 MiB memory and 4 MiB module caps; the extended-const-expr init exprs WAMR enables
    are evaluated once at load and are size-bounded by the module, not a hole. See
    `docs/bot-abi.md` §1.4 for the normative cost rule.
    Measured (Release, this machine): about 820 M instructions/s on an integer loop and 600 M on
    a float-heavy C bot, so a full 100 k budget costs about 0.12 ms and ten bots at full budget
    about 1.2 ms per tick, roughly 14× real time per core worst case; a `-O2` C bot that
    simulates every banana's flight uses about 63 k. Contests that need thousands of times real
    time run matches in parallel processes (WAMR's global state rules out threads) or lower the
    budget.
  - *Numbers.* `WasmAgentConfig`: `fuelPerTick` 100 000 instructions, `fuelInit` 10 000 000
    (applied separately to instantiation, i.e. `start` and constructors, and to `kfm_init`),
    `maxModuleBytes` 4 MiB, `maxMemoryBytes` 16 MiB (a declared minimum above it is refused, the
    maximum is clamped, `memory.grow` past it returns −1 and never traps), `maxTableElements`
    65 536 (a module whose tables would allocate more, counting a growable table's maximum, is
    refused at load), interpreter stack 64 KiB, host heap 0, `kfm_log` capture 4 KiB tail per
    agent. All host knobs (`--fuel`).
  - *ABI as built.* The exports are wasm `i32`/`i64` (`kfm_abi_version() -> i32`,
    `kfm_obs_buffer() -> i32`, `kfm_init(i64, i32)`, `kfm_act(i32) -> i64`); the only import
    allowed is `env.kfm_log(i32, i32)`, and importing memory is refused. Load validates size,
    imports, the four signatures and the memory, then runs a probe instance (under `fuelInit`)
    that checks the version and the buffer, so a bad module is refused at load, not mid-match.
    `init()` instantiates afresh every time, which is how `memory_on_respawn = wipe` falls out
    and what the host calls on a match restart. The buffer's host address is re-resolved every
    tick because `memory.grow` can move memory. Faults (`AgentFault`): `Trap`, `OverBudget`,
    `BadReturn` (bits 40..63 set), `Dead` (a fault in `init` leaves the agent dead and every
    later `act` yields no input without entering the module), plus the load-time kinds. A
    faulting `act` returns no input and the host repeats the slot's last input and counts it
    (`AgentHost`), the server's rule for a late network input. Compiled C and Rust bots keep
    their stack in linear memory behind a mutable `__stack_pointer` global and a trap skips the
    epilogue, so when a module exports that global the host saves it before `kfm_act` and
    restores it after a fault; SDKs export it by default (`-Wl,--export=__stack_pointer`).
  - *`Observation` v1.* 5748 bytes, alignment 4, little-endian, only u8/u16/u32/i16/i32 fields
    with explicit padding, every offset `static_assert`ed and the layout documented field by
    field in `docs/bot-abi.md`: header (version, size), match (tick, phase, clock, mode and arena
    indices, scores, deaths, respawn timers, the input delay, the content hash), a `rules` block
    of the content constants a flight helper needs (radii, speeds, flight ticks, turn rate,
    restitution, dash-attack numbers), self (kinematics, stats, tags, timers, the previous
    buttons, the three ability slots with the numbers of the def each runs, modifiers), the ten
    players (relation Self/Ally/Enemy, kinematics, hp, ammo, per-slot ability kind/phase/
    cooldown), the 64 bananas (phase, owner and its relation, heading as u16 BAM because a byte
    would make an exact SDK flight helper impossible, speed, radius, and the closest approach to
    a stationary self: the tick in 0..600 that minimises the distance and that distance, 0xFFFF
    while self is dead), the 32 pickups and the arena statics, each entry with a valid flag and
    the pools at their full capacity, and the v0.29 reservations (team and visible flags, heard
    calls, cell and facing octant, `turnTicks`, `ticksToLatch`, role, team scores, `modeData`)
    strictly zero (since v0.31 and v0.32 the role, `modeData` and the team fields are filled,
    below). Enum values may be appended within a version; a new `AbilityKind` trips a
    `static_assert` in the builder so the schema is reviewed. The builder is a pure function of
    (world, content, slot, delay) in fixed-point math only: `tools/ci/check_sim_no_float.sh`
    now covers `engine/ai`, so a bot match reproduces bit for bit on every host.
  - *Hosts.* `AgentHost` owns up to ten agents, fills a frame in slot order (present agent slots
    only, humans' and network inputs untouched), delays each answer by the slot's delay (at most
    120 ticks; the slot idles until the first answer is due; the line pauses while the slot is
    absent) and repeats the last applied input on no answer. It caches the content hash the
    observation carries per `ContentDB` object (recomputed after `attach`, `detach`, `reinit`
    and `clear`, or on `invalidateContentHash()`, which the client calls when live tuning edits
    its content in place): hashing every tick had been 60 % of a reference tournament and 95 %
    of a module run; `fill` is now allocation-free and O(agents). The reference bot's reaction delay
    is on top of the host delay and not compensated. Every host names bots by one grammar,
    `BotSpec`: `ref`, `ref:easy|medium|hard` or a `.wasm` path, and turns a spec into an agent
    through one `ai::BotFactory` (`kfm/ai/bot_factory.hpp`, its own target `kfm::ai_bots` so
    `kfm_ai` never sees WAMR: it links `kfm::ai_wasm` privately with `KFM_WASM_BOTS` and is
    otherwise reference bots only, a module spec an `Unsupported` error with no file opened). It
    reads each distinct module path once (at most `maxModuleBytes` + 1 bytes), loads it under the
    `WasmRuntime` it owns and gives every slot its own instance; the only knob is the per-tick
    fuel. What a factory error means stays the host's: `kfm_arena` exits 3 (4 for I/O),
    `kfm_server` refuses to start, the client falls back to the reference bot. Every host seeds
    slot `s` of a match with seed `S` as `S ^ (0x9E3779B97F4A7C15 · (s + 1))` so an arena run and
    a server match with the same seeds agree (`ai::agentSeed` in `agent_host.hpp`, the one copy
    every host calls).
  - *Reference bot.* Scores sixteen walking directions plus idle over a 32-tick horizon with
    enemy bananas predicted by the sim's own flight rule and the outer-box bounce, then takes,
    when they score, a dash-catch in the perfect window, a dodge (dash, then blink, parry or
    slam), a dash attack and a lead throw with a range and wall check; presses are fresh edges at
    sub-tick 0. Tiers `{reactionDelayTicks, aimNoise, catchPer256, throwPer256}`: easy
    `{18, 12, 48, 16}`, medium `{8, 5, 128, 64}`, hard `{0, 0, 256, 256}`. Two hard bots against
    two scripted players over ten 4-player matches took 42 rounds to 8 (85 eliminations to 6);
    four hard bots on `dojo`/`rounds` end a round in 3–4.5 s. Fairness pass (v0.30): equal
    target distances and equal movement utilities go to a random tied candidate from the bot's
    own Rng (reservoir sampling, a draw only on a tie) instead of the lowest slot or the +x
    direction; a dash attack is only started on a target that cannot answer with one on the
    same tick (unarmed, or its dash busy or cooling), because two strikes pressed on one tick at
    the same sub-tick are resolved by slot index, a coin the bot cannot win; in a standoff (the
    target holds a banana and its throw is ready) a lined-up throw is held 0..2 ticks, drawn
    once per lined-up shot, which is what breaks the mirrored openings of two noise-free hard
    bots (1v1 hard against hard: 0 of 20 matches capped and 68 % of rounds decided, from 13
    capped and 2 %); the wall band (2 units, penalised at 3 per unit) ends where an unarmed
    bot's fetch point lies, so a banana lying inside the band is still fetched (two bots had
    parked at the band's edge for ever). Residual slot bias is the sim's, not the bot's: with
    four players the dojo's spawn order puts slots 0 and 3 only 36° apart on odd offsets while 1
    and 2 face each other, so slots 1 and 2 win more whatever drives them (200 fixed-slot
    matches: scripted 28/65/72/35, hard bots 42/67/62/29; with four cyclic spawn points the
    same bots give 56/54/41/49, p ≈ 0.44), and equal-time Player × Player contacts resolve by
    pool index. Tournaments therefore rotate (`kfm_arena --rotate`): over 40 rotated matches of
    hard, medium, easy, hard the two hard bots win 20 and 19 with mean places 1.62 and 1.77,
    medium 1 and easy 0. The hard tier keeps zero noise. Pits (v0.57): a path that ends a tick
    with the whole disc over the pits outside a dash's active ticks is a fall (the sim's own
    `discWithinBoxes` over the observation's pit boxes), penalised like a hit (the dash-catch,
    the dodge, the blink and the dash attack check theirs too); a banana it could only reach by
    standing where it would fall is no fetch point; a falling enemy is no target; and when the
    walk the goal wants most, pits aside, falls in, a dash the same way that lands clear of the
    pit and of every banana takes it (continuous space; the grid's walks never enter a pit).
    Four hard bots on `island` over ten rotated matches: 2 of 166 eliminations were falls,
    against 78 of 197 with the pit terms off (with the first cut's centre rule, 0 of 162).
    The same run showed a standoff the grid had already met: two armed, mirrored bots parked at
    fighting range on either side of island's central block, neither with a line to throw,
    for a 33 019-tick match. The grid's clear-line goal (an armed bot's goal loses the preferred
    range when a wall blocks the line to its target) now applies in the continuous space too:
    island's ten matches average 2 768 ticks (5 480 before) with 0 falls in 190 eliminations,
    pillars 3 422 (3 839); dojo has no walls and is unchanged; corridors still caps 5 of 10, as
    it did before. An armed bot with a wall between it and its target also looks to the end of
    its horizon (32 ticks, not 16): at 16 no walk leaves a 2-unit block's face band, and two such
    bots stood still. With both, corridors caps 0 of 10 (mean 6 947 ticks), island averages
    2 915 with 0 falls in 184 eliminations, pillars 3 311. On the grid the bot now models a step
    into a pit cell as taken (the sim lets a walk in) for the step it commits to, so pitDanger
    scores it, where it used to model it as standing still: on courtyard at 1-tick turns, falls
    went from 51 of 95 deaths (27 of 112 on the old 2 × 1 pits) to 0 of 116. Mirrored hard bots
    still found standoffs where no move within the horizon clears the line (two bots
    half-turn-symmetric about island's central block, or behind courtyard's pillars), so an
    armed bot walled off from its target for `kStandoffTicks` (180, 3 s) of decisions throws
    anyway, a bank shot off the wall, and play moves on. With the final banana rule (dropped at a
    death), four hard bots over ten rotated matches: dojo, island, pillars, corridors and
    courtyard (continuous and 15-tick grid) cap 0 of 10; courtyard at 1-tick turns still caps 1
    of 10 (two grid bots toggling on one cell; the old map capped 2), a bot follow-up.

Teams, fog and calls (v0.29). The contest shape is a squad: a team of monkeys, each run by its
own program, seeing a slice of the world and talking through the game. One rule sits behind
every choice here: anything hard for a human by reflex is free for a bot, which has perfect
timing and can simulate the deterministic flight exactly, so bot balance comes from scarcity
(fuel, cooldowns, ammo), information (fog) and commitment (roots, travel time), never from
difficulty.

- **An entrant is a team.** The sim never learns that one author controls several monkeys: a
  team is the slots sharing a `MatchConfig::teams` index (§5.3). `kfm_arena --team
  a.wasm,b.wasm,...` fills a team's slots in order (one module repeated or several), and the
  result line reports per team and per slot. `kMaxPlayers` makes it five a side at most.
- **One module instance per monkey.** Isolated linear memory, its own fuel budget, and the four
  exports unchanged: no `slot` argument on `kfm_act`, no multi-input return. A trap or an
  over-budget tick costs that monkey its input, never the team's. An eliminated monkey's
  instance is paused and resumes on respawn; `memory_on_respawn = keep | wipe` is a host knob
  (`wipe` calls `kfm_init` again). Centralised planning needs no separate ABI: under team vision
  five instances of one deterministic module receive the same team picture, compute the same
  plan and each execute its own slice, at five times the fuel, which is the fair price.
- **Fog is a projection, not sim state.** The sim is full-information always, so determinism,
  rollback and replays are untouched and a replay is complete. `buildObservation` applies the
  mode's `vision` block (§5.10) per monkey:
  - `kind`: `all` (today's contract and every continuous mode), `team` (the union of the
    allies' sight), `own`.
  - `peripheral`: Chebyshev radius in cells around the monkey, seen whatever the facing
    (start 1).
  - `cone`: every cell whose direction from the monkey is within 45° of facing, inclusive, out
    to Chebyshev distance R. A cardinal cone is the band where the sideways offset is at most
    the forward one, a diagonal cone the closed quadrant, and both hold exactly R² + 2R cells,
    which a unit test asserts for all eight facings. Start at half to two thirds of the throw
    range and at least two turns of banana flight deep, so sight is worth more than sound.
  - `focus`: extra cone length for a monkey that took no commitment last turn (start 1):
    sentries see far, movers see near, and scouts have a reason to stop.
  - `hearing`: Chebyshev radius within which a banana in flight is observed whatever the
    facing (start: banana cells per turn plus 1, one turn of warning from any direction).
  - `banana_sight`: cells around an own banana in flight that its owner sees (start 1):
    scouting costs a banana and its flight time.
  - Line of sight: the cone yes, by integer Bresenham to each of its cells through the arena's
    walls (at most R² + 2R lines per monkey per turn); peripheral and hearing no.
  - Time: visibility is ORed over the ticks of a turn (the host steps every tick anyway), so a
    banana that crosses a cone inside one turn is seen; a one-tick turn is just the current
    tick. A banana's cell is the cell of its centre; the observation also carries its exact
    position and velocity, because the SDK flight helper is exact and a cell is an
    approximation.
  - Humans see everything: a client holds the whole world for rollback, so fog for a human is
    cosmetic and cheatable. Fog modes are bot-contest modes; the client draws no cone.
- **Calls: communication through the game.** A `call` ability kind (content: cooldown, `range`
  in cells on the grid or in units in the continuous space): on the press the monkey shouts its
  `aimDir` byte, 256 symbols, with no change to the 5-byte `PlayerInput`; the sim emits
  `MonkeyCalled{player, a = symbol}` and every monkey within range, ally or enemy, hears it, so
  interception is free and encoding is the author's problem. The observation builder folds the
  last turn's heard calls in (slot, symbol, tick, the caller's cell when visible); the client
  draws a shout, and a human on the ability button gets a ping. Position is a free channel too.
- **No shared array in v1.** An instant, global, lossless team array degrades fog to team vision
  with a one-turn delay, which the `vision` knob already offers. If squads prove too hard to
  write without it, it becomes a writable tail of the observation buffer read back after
  `kfm_act`, double-buffered so writes land next tick and instance order never matters, still
  behind four exports.
- **`Observation` v1 reserves** the fields these need, so the first fog mode is not a schema
  bump: team per player and its relation to self, a visible flag per player and banana (unseen
  entries zeroed, counts kept), the heard-calls list, self cell and facing octant, `turnTicks`
  and the ticks to the next latch, the role (loadout index), team scores and `modeData`.
  Filled since v0.31, with the meaning `docs/bot-abi.md` §3 gives them and no layout change:
  `self.reservedRole` (the match's loadout index until roles exist) and `match.reservedModeData`.
  Filled since v0.32: `players[i].reservedTeam` and `self.reservedTeam` (the slot's
  `MatchConfig::teams` entry; slot i in a free-for-all, so the fields are never "absent"),
  relation Ally in `players[i].relation` and `bananas[i].ownerRelation` (through
  `MatchContent::relation`), `match.reservedTeamScores[t]` (`teamScore`, which in a
  free-for-all equals `scores[t]`) and `self.reservedRole` per slot in a team mode with roles.
  `ObsModeKind` gains `TeamRounds` (3) and `TeamDeathmatch` (4), the tags bit 8 FriendlyFire,
  and a modifier's `ticksLeft` 0xFFFF means a mode's permanent baseline. A banana has no team
  field of its own in the v1 layout: its team is its owner's, reached through `owner` and
  `ownerRelation`. The reference bot targets enemies only (it did
  since v0.30) and, from v0.32, does not track a teammate's banana as a threat unless its own
  monkey holds `FriendlyFire`; it does not yet keep its throws off a teammate's line when
  friendly fire is on (follow-up).
  v0.33, appended values only: `ObsModeKind` `Hill` (5) and `Capture` (6), `ObsPickupKind::Flag`
  (2) with the flag's team in the pickup's `spawner` byte, tags bit 9 Carrying; the hill and
  capture `modeData` layouts are documented for `match.reservedModeData`. Zones are not in the
  v1 layout (a capture bot finds the flags in `pickups`; the zones wait for step 5's fields).
  The reference bot ignores flags and zones: it plays hill and capture as team deathmatch.
  Filled since v0.35 (as built below): `header.schemaFlags` bit 0 under fog,
  `players[i].reservedVisible` and `bananas[i].reservedVisible`, `reservedHeardCalls[16]`,
  `self.reservedCellX/Y` and `reservedFacingOctant`, `match.reservedTurnTicks` and
  `reservedTicksToLatch`; `ObsAbilityKind::Call` (6) is appended. Every reserved field of the
  v0.29 list is now filled; zones are still not in v1.
- **Contest knobs** are content and host settings, never code: team size, `vision` and its
  radii, call range, cooldown and alphabet (a mask over the 256 symbols), memory on respawn,
  fuel per instance, `turnTicks`, the catcher's cooldown. Tournaments pick rows; the engine
  stays one.

As built (v0.35, M5b step 5; `kfm/ai/vision.hpp`, `src/vision.cpp`,
`src/observation_builder.cpp`, `src/agent_host.cpp`, `src/reference_bot.cpp`,
`apps/arena`). Where it pins or adds to the design above:

- **Geometry.** Cells are `kfm/sim/spaces/grid_cells.hpp`'s; a monkey's, a banana's and a
  pickup's cell is the cell of its centre; the facing octant is `octantOf` of the aim byte (a
  half-way byte rounds counter-clockwise, as a `moveDir` does). `inCone(dc, dr, octant, R)` on
  column and row offsets (rows grow southward): a cardinal cone is `forward >= |side|`, a
  diagonal one the closed quadrant, both within Chebyshev R and without the own cell; a unit test
  counts R² + 2R cells for every octant and R up to 8 and checks both shapes cell by cell. A
  cell is seen by peripheral sight (Chebyshev ≤ `peripheral`; the own cell always, radius 0
  included), by the cone (R = `cone`, plus `focus` when focused) behind a clear line, or by
  banana sight (Chebyshev ≤ `banana_sight` of the cell of one of the monkey's own bananas in
  flight). Seen: every monkey and pickup whose cell is seen (pickups follow the cells: the
  design said nothing, a power-up or flag in a hidden cell is unseen), every banana whose cell is
  seen, every banana in flight within Chebyshev `hearing` of the monkey's cell whatever the
  facing and the walls, and the monkey's own bananas in flight.
- **Line of sight.** The integer Bresenham line from the monkey's cell to the target, both ends
  included: every cell after the first must be one of the arena's columns and rows and not a
  wall cell (its centre in a closed wall box), so a wall cell is never seen; a diagonal step
  between two wall cells that touch at a corner is blocked, as a walk cannot cut that corner;
  pits and mud never block. Bresenham from A to B is not always B to A reversed, so sight is not
  symmetric at every corner, which a bot may rely on.
- **Focus.** "No commitment last turn" is read from the state: a monkey is focused on a tick
  when it is at rest and did not move on the previous tick (`moveTicks == 0` and `pos ==
  prevPos`). A grid slide moves on every tick of its turn, the last included, so a monkey that
  walked or dashed is not focused at the next latch and one that stood or threw is; a push
  cancels it.
- **Kinds.** `own` is the monkey's sight alone, its allies included only when seen (a team
  shares what it knows through calls); `team` ORs the sight of every slot of the observer's team
  that has a monkey, so allies are always seen and a dead observer still sees through them;
  `all` is the v0.34 observation byte for byte (the visible flags stay 0, `schemaFlags` 0), and a
  test pins that.
- **The projection.** Under fog `schemaFlags` bit 0 is set; self and every seen monkey have
  `reservedVisible` 1; an unseen monkey keeps only its roster fields (`present`, `relation`,
  `reservedTeam`, `respawnTicks`), `alive` and everything physical 0, while `match.aliveMask`
  and `aliveCount` still count it (the scoreboard is public); an unseen banana or pickup is all
  zero, a seen banana has `reservedVisible` 1. Integer arithmetic only (the no-float check
  covers `engine/ai`).
- **The turn window.** `AgentHost::fill` computes every monkey's own sight once per tick when
  the mode has fog and an agent acts (`sightOf`), and each slot ORs its vision (`visionOf`: own,
  or the team's union) into its window, keyed by pool handle so a pool slot reused within the
  turn by an unseen entity stays unseen; the observation shows the current state of everything
  seen on any tick of the window. The window closes after the observation of a tick that ends
  a turn (`turnWindowEnds`: a Playing latch tick, `ticksInPhase % turnTicks == 0`, and every
  tick outside Playing, of a continuous mode or of a one-tick turn), so a latch observation
  carries everything seen since the previous latch and a one-tick turn is the current tick.
  `buildObservation` without a context projects the tick alone (tests, tools).
- **Calls.** Every host calls `AgentHost::observe(events, the world after the step, content)`
  after each `step`: `kfm_arena` and the client's `MatchSession` through `AgentHost::step`
  (fill, `sim::step`, observe in one call, so they cannot step without it), `kfm_server` from
  its per-tick hook after `NetServer::tick` (whose `botInputs` callback is the fill); netsim and
  `kfm_replay` run no agents. `fill` checks it: when the world has moved past the previous
  fill's tick, the last `observe` must have been handed the world at this tick (the one the
  step left); a miss is counted (`missedObserves()`) and aborts a build with asserts. Filling a
  tick again, a world that went back (a new match, a restart), ticks stepped and observed
  without a fill (no agent present), paused slots and ticks a host does not run (the room's
  Waiting and Over) never trip it. Each slot's `AgentSlotStats::heard` counts the calls that
  entered its window, and one host test per host (`kfm_arena`, `ServerHost`, `MatchSession`)
  plays a `grid_rounds_1t` squad of reference bots and requires that they hear some. A `MonkeyCalled` whose caller is still in the world is
  heard by every other slot with a live (not paused) agent whose monkey stands within the call's
  range, Chebyshev between the centres in units, after the step; it enters that slot's window
  (16 at most, later ones dropped), so a call made on tick T is in the observations of T + 1 up
  to and including the next latch, then gone. An entry is `valid`, `slot`, `symbol`, `tick` (of
  the call) and the caller's cell at the call when the arena has cells and the caller is seen in
  the observation that carries it (always without fog), else `kObsNoCell` (−1, −1). A caller
  does not hear itself. The alphabet mask is not built (a follow-up contest knob).
- **Grid fields.** In a grid mode on an arena with cells: `match.reservedTurnTicks` =
  `turnTicks`; `reservedTicksToLatch` from `turnClock` (Playing: `(turn − k mod turn) mod turn`;
  RoundStarting: `roundStartTicks − k`; RoundEnded: `roundEndTicks − k + roundStartTicks`;
  Lobby: `roundStartTicks`; MatchEnded 0; a round that ends early moves it); while alive
  `self.reservedCellX/Y` and `reservedFacingOctant`. `ObsPlayer` has no cell field in the v1
  layout, so only the own cell is reported; another monkey's cell is the SDK's `cellOf` over
  its exact position. A continuous mode leaves all of them 0.
- **Per-instance lifecycle.** `AgentHost` pauses a slot whose monkey was in the world since
  `attach`/`reinit` and is gone (eliminated, waiting for a respawn or the next round): no
  `act`, an idle input, a `paused` counter (`acts + paused` = the present ticks); a slot that
  never had a monkey yet (the lobby tick, a mid-round joiner) acts as before, so a host's
  first frames do not change. On resume the delay line and the window start empty and, under
  `RespawnMemory::Wipe`, `init(seed)` runs again (a module gets a fresh instance, counted in
  `wipes()`). The knob is `--memory-on-respawn keep|wipe` on `kfm_arena` and `kfm_server`
  (default keep; the server's stats line adds `paused=+N`); the client keeps.
- **`kfm_arena --team LIST`** (repeatable, one per team of a team mode, exactly the mode's
  team count, instead of `--bot`/`--bots`): entrant e's comma-separated specs fill its team's
  slots in slot order (the hosts' round-robin split: slot s is team s mod T), the last spec
  repeating, every team as large as the longest list; `--rotate` puts entrant e on team (e + k)
  mod T in match k, so every entrant plays every side. The result line adds `winner_team T` and
  one segment per team before the slots (`team T entrant E SPECS place P score S kills K deaths
  D`: team places by the `MatchEnded` winner's team, then team score, then fewer deaths, then
  team index; kills count enemies only), and a slot segment its `team`; the summary adds
  `entrant E SPECS wins W mean_place X ...`; the JSON mirrors it (`winner_team`, `teams`,
  `entrants`). A slot's bot index is entrant × team size + its position.
- **The C SDK** (`tools/bots/sdk-c/kfm_bot.h`) names the filled fields without the prefix
  (`turnTicks`, `ticksToLatch`, `teamScores`, `modeData`, `facingOctant`, `role`, `team`,
  `cellX`/`cellY`, `visible`, `heardCalls`), adds `KFM_ABILITY_CALL`, the hill, capture, flag
  and Carrying values, `KFM_NO_CELL` and the meaning of `KFM_SCHEMA_FLAG_FOG`; every offset
  assert is unchanged and `example_bot_test` checks the renamed fields against the engine's.
  The example bots read none of them; `build_example.sh` rebuilds `chaser.wasm` and
  `idle.wasm` byte for byte with the emsdk clang.
- **The reference bot on the grid and under fog** (all of it gated on a grid turn clock or the
  fog flag, so every continuous mode plays bit for bit as before): it walks the eight octants
  and idle from the next latch on, a cell per turn by the sim's step rule (the target cell free
  for its circle, both orthogonals free for a diagonal, pits never free); goals are measured by
  walking distance (a BFS over the free cells, 64 × 64 at most, cached per goal cell), so a wall
  between it and its target or its banana is no longer a resting point, and an armed bot wants a
  clear throwing line; it fights at two cells with its throw capped at three, its flight speed
  learned from the bananas it sees (Observation v1 carries only the base speed) and its cell
  size from its own cell at rest; dashes, the catcher's catching dash included, go along
  octants. Under fog it acts on what it sees (a zeroed entry is never a monkey or banana at
  the origin) and keeps an unseen owner's returning banana flying; with nobody in sight it heads
  for the last enemy it saw or an ally called out (600 ticks), else the arena's centre, its cone
  turned there and sweeping a quarter turn every four turns once there. With a call on the
  ability button it calls out a seen enemy's coarse cell, symbol `(col / 2) | (row / 2) << 4`,
  at most once per eight turns and, at 15-tick turns, never on a latch tick. Measured (Release,
  this machine): `kfm_arena --mode grid_rounds_1t --bots 4 --matches 5` ends 5 of 5 (572–1591
  ticks), `--mode grid_rounds --matches 3` 3 of 3 (1064–1746), from 0 of 8 in v0.34; 20 rotated
  5v5 matches on `grid_rounds_1t` under `own` fog all end (mean 1140 ticks, 10 wins a side) at
  about 30 k ticks/s with ten bots. A playable baseline, not a strong bot: it does not plan for
  its team, read calls beyond the last sighting, or use the catch on purpose.

**The ladder (v0.36, M6b then; M7 since v0.40).** The ladder is the hosted contest: authors submit bots, the service
plays them against each other with `kfm_arena`, ranks the entrants and serves the replays. It
lives in the bots repository (above) and uses this repository through its tagged releases (one per match, v0.59). Decided:

- **Accounts, tokens and teams.** A user has an account and a personal bearer token, created and
  revoked on the portal and stored by the CLI after `login`; it is scoped to submit and read. An
  entrant is a team of users (a solo player is a team of one), and a team's membership is a
  table, so a member leaves without rotating a shared secret. Submissions, the active bot and
  the rating belong to the team; the submitting user is recorded on each submission. Identity is
  a hosted OAuth provider, never a password the service stores: GitHub or Google (v0.63), an
  account may link both, and the account is keyed by the provider's subject id, never by the
  email (an email changes and two providers may report the same one).
  **An account is in at most one team (v0.78)**, as owner or member: the entrant and the rating
  are the team's, so an account in two teams would be two entrants, holding two places on the
  board and paired against itself. The service refuses the second membership when it is made
  (creating a team, or being added to one) and leaves the ones made before. The exception is an
  **operator**, the account of whoever runs the ladder: a round needs two teams with a ready
  bot, so the ladder runs **house teams** (the bots repository's `examples/starter` and
  `examples/holder`, submitted through the public path like any entrant's) for a first
  submission to be paired against, and one account owns them. The flag is on the account, set
  on the service's VM like a season (`kfm-ladder operator add`), never over HTTP, and lifts
  this limit and nothing else: there is still no admin role over the API.
- **Source in, module out.** A submission is a source tree plus a small manifest (the entrant,
  the language, the build entry), uploaded as a zip through the portal or through the CLI; the two
  paths hit one HTTP endpoint. The service builds it, never the author: every ranked module then
  comes from the pinned toolchain, the source is on file for auditing and for publishing after a
  season, and a module is byte-identical to a local build of the same recipe (`build_example.sh`
  already shows this for the emsdk clang). The only ladder language is C++ (the C SDK header is
  C11 that compiles as C++; a C++ convenience layer lives in the SDK). The recipe is the SDK's:
  emsdk clang++ over every source file in one command with the flags of `docs/bot-abi.md` §5,
  no third-party packages (the SDK has no dependencies, so an offline build is the rule, not a
  restriction). Prebuilt modules are not accepted on the ladder.
- **The build sandbox.** Each build runs in a throwaway container: a fresh container from the
  pinned per-language image, destroyed when the build ends, with no network, the source mounted
  read-only, a read-only root with a tmpfs scratch, an unprivileged user, and caps on CPU, memory,
  process count and wall time (`docker run --rm --network none --memory 2g --cpus 1
  --pids-limit 256 --read-only --tmpfs /tmp --user builder -v SRC:/src:ro -v OUT:/out IMAGE
  timeout 120 recipe`, or the podman equivalent). The only result that counts is one `.wasm`
  in the output directory; a limit hit is a failed submission with its log. A public service runs
  the containers under gVisor or on a build host that holds nothing else. After the build the
  service validates the module exactly as `WasmAgent` does (ABI version, the 4 MiB module and
  16 MiB memory caps, instantiation and `kfm_init` under the 10 M budget) and plays one smoke
  match against the reference bot; a module that traps or runs over budget in it is rejected.
- **Submissions and retention.** A submission is immutable: the source archive, the built module
  and its hash, the build log and the validation result. A team keeps the five most recent
  submissions plus whichever is active (the active one is never pruned); the CLI and the portal
  list them and set the active one. Built modules are kept for the whole season regardless, so
  every ranked match stays reproducible from seed plus modules; a source archive is small and a
  module is at most 4 MiB, so the cap is about tidiness, not storage.
- **The `contest` mode.** The season's ruleset is one mode in the content, `contest`, the
  ladder's row of the contest knobs above (team size, `vision`, call range, `turnTicks`, fuel,
  memory on respawn) on its arena; since v0.69 it is king of the hill (`kind = "hill"`) with
  `grid_rounds_1t`'s squad on `courtyard_hill` under `own` fog with calls (before it, that row
  renamed: team elimination on `courtyard`). Bots find the hill in `match.reservedModeData`
  words 8–15 (the arena's zones 0 and 1, bot-abi.md §3). Designed in v0.79 and not yet built
  (§11 "Contest design"): the row becomes `control`, three zones to capture and keep on a new
  arena, with a faster banana and early power-ups. The ladder points at the name, so a season
  changes the row, not the service, and the row may change between releases inside a season (v0.59). **A season is a
  time window, not a release** (v0.59; until then a season pinned one release and its content
  hash): the ranking freezes at a date, the service runs every match on the current tagged
  release of this repository and records that tag on the match, and the content directory is
  kept per release because replays carry the content hash and a viewer or `kfm_replay verify`
  needs that exact content. A release changes the game for every entrant at once; the service
  announces it to them, and the CLI's `test` runs the current release by default so an author
  sees a bot broken by the change before the scheduler does.
- **Matches and ratings.** The service schedules two-sided matches between active submissions
  with `kfm_arena --mode contest --team A --team B --rotate --json --replay-dir`, deterministic
  seeds recorded per match, and parses the per-match JSON object (`match`, `seed`, `ticks`,
  `checksum`, `winner_team`, `teams[]` with `entrant`, `place`, `score`, `kills`, `deaths`,
  `slots[]` with the module's `traps`, `over`, `bad`, `fuel_*` and `dead`); from v0.36 that
  object's keys are append-only, like the ABI. The rating is per entrant (switching the active
  submission keeps the rank), Elo or Glicko-2 over the two-sided pairs, with the submission ids
  on both sides and the release tag recorded on every match so a disputed result is re-run bit
  for bit with that tag's `kfm_arena` from the GitHub release (v0.59). Ratings persist across a
  release inside a season (v0.59): an update is the same shock to every entrant, and a reset
  would charge the leaders for the project's own changes; under Glicko-2 the rating deviation
  widens at a release instead. The free-
  for-all slot bias of §12 does not reach the ladder: `--rotate` plays every pair from both sides.
- **Replays.** A match's `.kfmr` is served with the content of the release it ran under (v0.59;
  its season's until then); the CLI downloads it and prints the viewer URL, and the browser
  client plays a replay by URL (v0.68, M7 step 6, §7 "Replay viewer": `?replay=<url>` is the
  client's `--replay`, fetched with CORS from the bucket, which allows GET from the page's
  origin; it needs no bot runtime, since a replay is inputs). The viewer is the client of the
  replay's own release (v0.59): a sim change between releases can replay differently even when
  the content hash did not move, and the header carries both the content and the build hash, so
  the release workflow publishes the page under a versioned path per tag (`/v0.60/`) beside the
  root, which serves the newest for play, and the ladder's link names the match's tag. The
  viewer refuses a replay whose content hash is not its own and names both; the build hash is
  shown, not checked, until a build stamps one (v0.60). A page that predates the viewer (every
  release up to v0.5.1) passes `?replay=` on as well, and its client refuses `--replay` as an
  unknown argument and exits before the first frame: those matches are not watchable, by
  design. Fog is a
  projection in the observation builder, so a replay shows the whole arena; a fog overlay in
  the viewer is still a follow-up, and so are the entrants' names (the portal knows them; a
  `?names=` parameter is the likely shape).
- **The CLI.** One static binary written in Rust, released as platform wheels through PyPI with
  maturin (`uv tool install kfm-cli`, `uvx --from kfm-cli kfm`; the PyPI project is `kfm-cli`,
  v0.66) and on the bots repository's releases page.
  Commands: `login`, `build` (the recipe in the pinned image, locally), `test` (a match against
  the reference bot through a released headless `kfm_arena`), `submit`, `bots list` and
  `bots activate`, `matches list`, `replay get` and `replay open`.
- **What this repository contributes.** `kfm_arena` as the engine with its append-only JSON
  result (`docs/arena-result.md`, normative since v0.60), the `contest` mode in content, the browser client's replay-by-URL viewer (v0.68), a released
  headless `kfm_arena` binary per tagged version, and `docs/bot-abi.md` as the normative ABI.
  The portal, the accounts, the build images, the scheduler, the ratings store and the CLI are
  the bots repository's.

---

## 9. Tooling, testing, quality

- Tests: Catch2 unit tests per system; scenario tests (scripted inputs → expected events and
  state); property tests (random inputs, invariants such as no entity leaks and bounded
  values); netcode tests over `LoopbackTransport` with 0–300 ms latency and 0–10% loss; libFuzzer
  on packet decoders; Google Benchmark for ticks/s and rollback cost. Until there is more than
  one number to track, `kfm_replay bench` stands in for Google Benchmark (v0.13): it times
  `step()` over scripted matches (with and without input generation), restarting a match that
  ends so every timed tick is live gameplay.
- Client smoke test (v0.15): `kfm_client --exit-after-frames N [--screenshot out.ppm]` renders
  N frames and exits, so rendering is checked without a human: on Linux CI a `client_smoke`
  ctest runs it (`--autostart --bots 3 --exit-after-frames 60`, a started match) under
  `xvfb-run` with Mesa's software rasteriser (X11, `LIBGL_ALWAYS_SOFTWARE`), registered only
  when `xvfb-run` exists at configure time. A renderer that cannot create its buffers, shaders
  or pipelines makes the client exit non-zero, so the test fails on a broken renderer; a
  missing character model only warns and draws capsules. The browser page sets
  `webglContextAttributes` (`powerPreference`, no antialias; v0.56: no `desynchronized`) and,
  because SDL3 creates its context through `emscripten_webgl_create_context` which ignores that setting,
  also wraps `getContext('webgl2')` to merge the same attributes.
- Audio and VFX tests (v0.19): `kfm_presentation_tests` checks every event's effects and cue,
  effect lifetimes and expiry, dash and banana trails, entity positions of pickup and landing
  effects, the count-in/go sequence, same-frame cue merging and pan, banner pop-in, and over a
  scripted 10-player match that every new effect kind and sound appears with pans and gains in
  range; the synthesiser (every clip's length, finite, normalised under the peak, faded ends,
  deterministic, sample-rate aware) and the mixer (gain × clip gain × volume, constant-power
  pan, voices ending, silence when muted or at volume 0, stealing the oldest voice, the
  [−1, 1] limiter, no bank). `kfm_client_tests` checks that an event a rollback retracted
  before it was drained spawns no effect or sound and that a re-simulated duplicate plays once,
  through the real `EventJournal`, plus the `--mute`/`--volume` flags. `client_smoke` and
  `--exit-after-frames --screenshot` run unchanged without an audio device.
- Feel-tuning instrumentation (v0.15): `kfm_replay stats` (and `record-scripted --stats`) runs
  scripted matches and reports round lengths, throws, hits, dashes, perfect and auto catches,
  catch attempts and their conversion, `tHit − pressTime` for every attempt, throw-to-outcome
  times, time spent without ammo and the banana supply floor; v0.16 adds catches declined at
  full ammo and dash attacks (dashes that met a hittable player, with and without ammo, hits and
  eliminations by dash). The catch inputs exist only
  inside `step()`, so the tool runs the ten systems itself and resolves the contact list one
  contact at a time in `InteractionSystem`'s order, judging each against the state the rules
  table sees (a contact whose banana or player an earlier contact of the tick removed or
  eliminated is not counted, and nothing counts outside `Playing`); a shadow world advanced by `step()` is compared by checksum every tick so
  the mirror cannot drift from the sim, and no instrumentation lives in `engine/sim`. The
  thresholds the tool reads (the dash attack's `min_ammo`, each dash's perfect window, the
  summary's window label from the match loadout's dash) come from the match's content, so
  `--content` and edited TOML are measured with their own numbers; four `replay_stats_smoke*`
  ctests (a free-for-all, power-ups with slam in deathmatch, `island`'s pits and the `contest`
  squad) run the shadow check in CI.
- Browser check (v0.15): `tools/web/serve.py` serves the wasm build (`--page`, optional
  COOP/COEP headers) and `tools/web/check_browser.py` drives it in headless Chromium through
  Playwright (page loads, no console errors, WebGL2 context attributes, frame intervals,
  keyboard input, screenshots). It is a developer tool, not a CI step: a browser download per
  run and timed waits would make the gate slow and flaky, and `client_smoke` already covers
  rendering on CI.
- Replay viewer tests (v0.68): `tests/unit/client/replay_session_test.cpp`
  (`[client][replay]`, in `kfm_client_tests`) plays every corpus replay (22) to its end at one
  tick per frame with every tick's checksum equal to its `.checksums`; checks that the frame
  events at three ticks per frame equal a direct step loop's; seeks (forward lands on the
  recorded checksum, backward then play reaches the same final checksum, past the end clamps,
  restart); keyframes (seeks from the end to ticks 0, 1, 599, 600, 601, 750, 1199, 1200, 1799
  and 1800 take exactly `target % 600` steps and land on the `.checksums`, and forward seeks
  record keyframes); the interpolation policy below and above 1x and the newest-state-alone
  drawing while still, paused or at the end; pause, step and speed (each speed steps 120 × speed
  ± 1 ticks over the same wall time); every refusal of `load()`; and `readFileBytes`. 17 cases,
  1856 assertions under `[replay]`. The corpus path is the `KFM_REPLAY_CORPUS_DIR` compile
  definition; under Emscripten the corpus is embedded with `--embed-file
  tools/replay-corpus@/replay-corpus`, so the wasm run under node plays it too, and
  `kfm_client_tests` has `LINK_DEPENDS` on the corpus (a `CONFIGURE_DEPENDS` glob) so a
  regeneration relinks it. `replay_controls_test.cpp` covers the key and pad mapping, Shift,
  clamping, Watch again, Step enabling, the focus wrapping and skipping, `refocus` and hovering
  a disabled control, `replayBarRows` at 800, 852, 1024 and 1280 px, scrubbing and the labels;
  `lobby_test.cpp` the `--replay` parsing and its exclusions. Two ctests beside `client_smoke`,
  registered the same way, both by `PASS_REGULAR_EXPRESSION` (so the exit code is not what they
  check): `client_smoke_replay` (60 frames of `scripted_10p_contest_1800t.kfmr`; the exit line
  must match `in the replay at tick [1-9][0-9]* of 1800,`, so the replay advanced) and
  `client_smoke_replay_bad` (`corpus.txt` as a replay; the output must match `error: replay
  [^\n]*corpus\.txt: not a replay.*the replay could not be shown`).
- `kfm_netsim` (v0.17, `apps/netsim`, over the shared `kfm::netsim` harness in
  `engine/net/netsim` that `kfm_netcode_tests` also uses): N clients and a server in one process
  over the loopback emulator, `--clients --rtt --jitter --loss --dup --reorder --seconds --seed
  --input-delay --spectators --join-at --cut C:D[:AT] --csv`, a per-client-per-second CSV and the
  soak summary line, exiting non-zero on any mismatch, resync, rollback overflow or a client not
  running at the end. Two ctests (label `soak`, 30 s of match each, identical numbers in Debug
  and Release because the runs are deterministic): `netsim_10c_100ms_2pct` (depth max 11 / p99 9
  / p50 8, 50 mispredicted ticks/s, lead 4.65 against 5.24, 0.035 substitutions/s, 1.1 KB/s up
  and 4.1 KB/s down per client; 0.3 s Release, 2 s Debug) and `netsim_10c_300ms_10pct` (depth
  24 / 22 / 20, lead 10.7 against 11.2, 0.47 substitutions/s, 1.4 / 7.2 KB/s; 0.7 s Release,
  3.9 s Debug), both with zero mismatches, resyncs, overflows and hard jumps. `kfm_server_tests`
  plays a full bot match to the end over the loopback and through the restart;
  `kfm_server_network_tests` (label `network`) runs `ServerHost` over a real `WebRtcTransport`
  with two in-process clients on the real clock (running after about 115 ms, 0 mismatches,
  shutdown seen by both). The transport contract suite runs over `WebRtcTransport` in
  `kfm_net_webrtc_tests` (11 cases, about 2.6 s, 20/20 consecutive runs clean). CI gains a
  `server-headless` job (Ubuntu 26.04, Release, FetchContent) that builds `kfm_server` and runs
  its ctest; the native matrix runs the `soak` and `network` labels through `ctest`.
- Browser network check (v0.17): `apps/net-probe` (`kfm_net_probe --serve PORT` natively,
  `--connect ws://host:port` natively, `kfm_net_probe.html?connect=...` in the browser) runs a
  `WebRtcTransport` echo exchange: idle pings for RTT, then N messages per lane of varied sizes
  including each lane's maximum and one oversize message per lane that must be refused, and
  prints `KFM_NET_PROBE PASS|FAIL ...` (also in `window.kfmNetProbeResult`).
  `tools/web/check_webrtc.py` starts the native server, serves the wasm page and drives it in
  headless Chromium; `net_probe_loopback` (`--loopback`, label `network`) covers the probe
  natively in ctest. First browser run: 200/200 on each lane, 0 lost, 0 corrupt, 0 out of
  order, idle RTT median 1.2 ms, connect 98 ms, no Chromium flags (the browser's host candidate
  is an mDNS name; the server's is its interface address, so a server behind NAT needs STUN or
  a public address). It found two datachannel-wasm link defects fixed at the build level
  (§10): its JS glue uses `_malloc`/`_free` without declaring them (a forced dependency stub,
  `engine/net/src/webrtc/datachannel_wasm_deps.js`) and reads `Module['HEAPU8'|'HEAPU32']`,
  which Emscripten 4+ only exports on request (`-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32` on
  `kfm_net_webrtc`'s interface link options; the setting is last-wins, so an executable that
  sets its own list must keep both). Linking the transport into `kfm_client.html` costs about
  4 KB gzipped (23 KB once referenced) and nothing in frame pacing.
- Lobby and deployment (v0.38): the Rust service has 4 unit and 18 integration tests
  (tokio-tungstenite clients against the lobby in-process on port 0, covering every refusal
  reason, the relay in both directions, expiry, capacity, the rate and size limits and the
  first-message timeout) and a `lobby` CI job (fmt, clippy with `-D warnings`, test, all
  `--locked`). `.github/workflows/deploy.yml` runs on `v*` tags and by hand: it pushes the
  `kfm-server` (the `server-headless` preset in an Ubuntu 26.04 builder, `kfm_server` and
  `kfm_arena` with `content/` in a non-root runtime image) and `kfm-lobby` images to GHCR and
  publishes the wasm client to Cloudflare Pages with `config.js` and `_headers` generated from
  repository variables. `deploy/` holds the compose file for local development, the systemd
  units (`kfm-lobby.service`, `kfm-server@.service` over Docker with host networking), the
  Caddyfile, `install.sh`/`update.sh`, the `gcloud` scripts and the Hetzner equivalent;
  `deploy/README.md` is the runbook. Since v0.47 a tagged release also updates the VMs
  from the workflow: `vms` runs between the images and the page, ships `deploy/vm/` to each VM
  named in `KFM_VMS` over an IAP tunnel and runs `update.sh TAG` (a rolling restart at
  once; `--drain` is opt-in), authenticated through Workload Identity Federation rather than a stored key, so
  the page never serves a build the servers refuse; a manual run skips the VMs. Since v0.73
  the page is built beside the images (`web-build`) and `web`, after the VMs, only publishes
  it: a client of another build id is refused and told to reload (v0.70), so until the new
  page is up a player on the old page reloads into the old page.
- Determinism gate in CI: replay corpus → identical per-tick checksums across Linux, Windows,
  macOS, Debug and Release. A replay carries the content hash and `kfm_replay` refuses one whose
  hash differs from the build's (no legacy exemption), so the corpus is defined by a checked-in
  manifest, `tools/replay-corpus/corpus.txt` (name, kind, seed, input seed, players, ticks), and
  `regen-replay-corpus` re-records every entry before regenerating its `.checksums` (v0.14). A
  content or gameplay change regenerates the corpus in the same commit; a `.kfmr` without a
  manifest line fails the regeneration. v0.18: replays (format version 2) store the arena and
  mode indices, the manifest takes two optional trailing columns (arena and mode names, default
  the first of each), and `kfm_replay` records and plays back with the embedded content unless
  `--content` says otherwise. Adding any def (an arena included) changes the content hash, so
  the corpus is re-recorded; the checksums of existing entries must stay identical, because
  content that is only added never changes the default match (a scenario test asserts this for
  extra arenas and modes). v0.70: the gate proves an entry reproduces, not that it still
  exercises what it is in the corpus for, and the corpus is scripted input a seed or content
  change can quietly move. `kfm_corpus_tests` (`tests/determinism/corpus_events_test.cpp`,
  label `determinism`) plays every manifest entry against the embedded content and asserts the
  events its name promises (the island entry a `PlayerFell`, blink/slam/parry their ability's
  event, the power-up entries a power-up taken and the powerset one only its two power-ups
  spawned, deathmatch and team deathmatch a scoring elimination and a respawn, the team and
  round entries a round won, hill and contest the hill held and a point, capture a flag taken
  and a capture, the grid squad and contest a call, the empty entries no action at all) and no
  `EventSink` overflow; a manifest line without a row there fails, so a new entry states what it
  is for. v0.22 adds one 4-player scripted replay per walled arena
  (`scripted_4p_pillars_1800t`, `scripted_4p_corridors_1800t`, `scripted_4p_island_1800t`), so
  wall slides, bounces, pit falls and slow patches are under the cross-platform gate. The
  embedded content depends on the globbed file list as well as the files (a list file rewritten
  only when it changes), so adding, moving or deleting a TOML file re-embeds before a regen.
  v0.24: replays (format version 3) store the per-match power-up set; a ninth optional column
  names it (`all`, `none` or comma-separated names, default all), and
  `scripted_4p_powerset_1800t` (dojo_powerups with only shield and big_banana) keeps the
  intersection under the gate.
  v0.31: replays (format version 4) store the loadout index (`kfm_replay info` prints it); a
  tenth optional column names it (`kfm_replay --loadout`, default `base`), which needs the
  ninth, so a loadout line reads `... dojo rounds all slam`; lines with 6, 8 or 9 columns stay
  valid.
  v0.21 adds `scripted_4p_blink_1800t`, `scripted_4p_slam_1800t` and `scripted_4p_parry_1800t`
  (dojo, the `rounds_*` ability modes; since v0.31 `rounds` with the `blink`, `slam` and
  `parry` loadouts): the scripted bots use the third slot when it holds an
  ability (blink sideways out of an incoming banana's path or now and then, slam with an enemy or
  incoming banana within the reach, parry a close incoming banana) and draw nothing from their
  Rng while it is empty, so the base-kit entries keep their inputs. `kfm_replay stats` counts
  blinks, slams, pushes and parries (and now sums the power-up counters over matches). The third
  `AbilityState` and `Player::knockback`/`knockbackTicks` changed every checksum and
  regenerated the corpus.
  v0.23 adds `scripted_4p_deathmatch_3600t` and
  `scripted_4p_survival_1800t` (dojo, the shipped modes), so respawns, the mode counters and the
  survival flow are under the cross-platform gate. v0.20 adds `scripted_4p_powerups_1800t`
  (`dojo_powerups`, scripted bots that walk to power-ups), so pickups, grants, projectile
  modifiers and the shield are under the gate too. New `WorldState` fields (v0.23: the mode
  counters; v0.20: `Boomerang::speedMul`/`radiusMul`, `Pickup::spawner`,
  `Player::grantedTags`) change every checksum and regenerate the corpus; added content alone
  must not.
- Mode tests (v0.23): `mode_scenario.cpp` (deathmatch scoring, respawn exactly `respawnTicks`
  after the elimination at the farthest spawn point, spawn protection blocking a hit for
  `respawnInvulnTicks`, `scoreToWin`, the clock with the leader winning or a draw on a tied
  lead and the 10/3/2/1 warnings, late joiners and leavers; survival's last-standing and
  timeout tie-breaks; rounds never setting a mode counter; a mid-respawn round trip through the
  visitor; determinism), the counters' visitor round trip, loader validation (unknown kind,
  `score_to_win = 0`, `respawn_ticks = 0` in deathmatch), the HUD fields and mode effects and
  sounds, a server restarting a deathmatch and a survival match, a local deathmatch ending on
  its clock, and a 4-client deathmatch over the emulator at 100 ms / 2% staying in sync.
- Content tests (v0.18, `kfm_content_tests`): the exact decimal conversion (truncation, digit
  separators, range, per-second division), `content/` and the embedded copy agreeing with
  `defaultContent()` on the base kit, arena 0 and mode 0 (and being identical to each other), validation of `defaultContent()`, one diagnostic per kind of mistake
  (syntax with line, unknown key, missing section or key, bad name, duplicate, unresolved
  reference, ranges, capacities, contradictory spawn keys) and the app-side resolution.
  `match_config_scenario` checks that the selected arena places and bounds players and the
  selected mode decides the countdown and the match end; a netcode test runs a server on a
  second arena and mode and restarts it into the default one with no checksum mismatch.
- Scripted inputs (v0.13): `kfm/sim/scripted_inputs.hpp` generates varied, deterministic
  matches (wander, aim at the nearest player, throw, dash at incoming bananas, fresh button
  edges) from an `Rng` separate from the world's. v0.16 adds a dash attack: while holding at
  least `DashAttackDef::minAmmo` bananas with another player within `dashAttackRadius` (3 units),
  a player dashes straight at it with chance 1 in `dashAttackOneIn` (240) per tick, so the
  determinism corpus exercises row 25. It is test and tooling scaffolding shared by
  the scenario tests, `kfm_replay record-scripted`, the bench and later the netsim, not an
  `IAgent` (§8); it lives in `engine/sim` only because it is float-free, allocation-free and
  never writes `WorldState`.
- Replays: `seed + match config + content_hash + input stream`. The client's viewer (v0.68,
  §7) seeks from periodic keyframes, one every 600 ticks, recorded as it plays. Replays attach
  to bug reports.
- Observability: Tracy zones; structured logging; net stats (RTT, jitter, loss, rollback depth,
  mispredictions/s) in the debug overlay; server metrics endpoint later.
- Code standards: C++23, warnings as errors, clang-format and clang-tidy in CI, ASan/UBSan
  builds, no exceptions or RTTI inside `sim`, strong typedefs (`Tick`, `PlayerId`, `EntityId`),
  `[[nodiscard]]`, no raw `new`.
- CI run time (v0.77). The wall time of a CI run is its slowest leg, so three things keep the
  legs short. vcpkg's packages are restored from a `files` binary cache that `actions/cache`
  keeps per toolchain (a leg's Debug and Release builds, and the sanitizer leg with
  `linux-gcc`, build the same packages), saved only when a configure built something the
  restored cache lacked. `ctest` runs four tests at a time on every leg; the tests labelled
  `network` open real sockets and are `RUN_SERIAL`, so a parallel run never starves a
  handshake, and the tests that start a virtual display share the `xvfb` resource lock. The
  lint job runs one `clang-tidy` per core over batches of files. None of this removes a test;
  the one thing a leg leaves out is the three `kfm_netsim` soak gates on the sanitizer leg,
  which the `server-headless` Release leg runs (the sanitizer leg still runs the same netcode
  in `kfm_netcode_tests`).
- Why C++23: the design uses C++20 directly (`std::span` in interfaces, `<bit>` for pools and
  serialization, `std::endian`, concepts to reject non-trivially-copyable types in `Pool` and the
  visitor, defaulted `operator<=>` on strong typedefs and `Fixed`, `consteval`, designated
  initializers for defs and rule tables) and two C++23 features earn the step: `std::expected`
  for fallible decoders (no exceptions in `sim` or `net`) and deducing `this` for CRTP-free
  visitors and pools. Avoided: modules (weak build-system and Emscripten support) and coroutines
  in `sim` (they hold state outside `WorldState`, which breaks rollback by `memcpy`).
- How it was decided (M0): `tools/cxx23-probe` exercises exactly those features. It passes on
  GCC 15.2, Clang 21.1, MSVC 19.51, Apple Clang 21 and Emscripten 6, and fails on GCC 13.3 (no
  deducing `this`) and Clang 18.1 with libstdc++ 13 (no `std::expected`), which are what Ubuntu
  24.04 ships. Linux CI therefore runs on Ubuntu 26.04. The requirement is a build machine with
  GCC ≥ 15 or Clang ≥ 19 (v0.62: GCC 14 was never in CI and fails at `-Werror`); the dedicated server's deployment is decoupled from the host distro by
  building in CI and shipping the binary (static libstdc++) or a container, so this is a
  build-machine constraint, not a hosting one. The probe stays in the build as a canary.

---

## 10. Write vs. buy

| Write ourselves | Take off the shelf |
|---|---|
| Sim, pools, fixed-point math, swept collision, abilities and rules framework | SDL3, sokol_gfx, Dear ImGui, stb |
| Message layer, prediction/rollback, clock sync, network emulator | libdatachannel + datachannel-wasm (transport); GameNetworkingSockets only for a Steam build |
| Bots, observation builder, the bot ABI and SDKs, match runner | LZ4, xxHash, toml++ or nlohmann-json |
| Replay tooling, content validator, glTF export script | Catch2, Google Benchmark, Tracy, libFuzzer, WAMR 2.4.5 (the wasm bot runtime: classic interpreter with instruction metering, §8), cgltf, Blender (asset pipeline) |
| The lobby service (Rust: Tokio, axum, tokio-tungstenite; rooms, tokens, signaling relay, server registry, §13.5) | Caddy (TLS), Cloudflare Pages (the client page), Google Cloud Compute Engine then Hetzner Cloud (VMs), Sentry free tier (browser and lobby errors), systemd and journald (process pool and logs) |

Dependencies via vcpkg manifest; CMake presets for `dev`, `release`, `asan`, `server-headless`, `wasm` (Emscripten).
M2 dependency set (v0.15): SDL3 3.4.18 (vcpkg at the baseline with features `core, wayland,
x11`, or FetchContent `release-3.4.18` built static with the same optional subsystems switched
off as the port, so both paths build the same SDL; SDL 3.4 aborts configuration on Linux when an
enabled optional dependency is missing; v0.19 adds the `alsa` feature for Linux audio, below);
Dear ImGui 1.93.0 (FetchContent only: the vcpkg `imgui[sdl3-binding]` port
re-enables sdl3's default `ibus`/`dbus` features and drags in libsystemd and meson); sokol
pinned to floooh/sokol `efeb776` (FetchContent only, the vcpkg port is a 2023 snapshot); glad2
2.0.8 vendored; cgltf 1.15 (vcpkg or FetchContent). FetchContent uses release tarballs with
SHA256 hashes. The Emscripten build compiles SDL3 from the same source.
When a package is not found through vcpkg (no `VCPKG_ROOT`, or the `wasm` preset, which sets
`KFM_USE_VCPKG=OFF`), CMake falls back to FetchContent pinned to the same version as the manifest,
so every build uses the same release of each dependency. The lookups enforce the pin rather than
taking whatever is installed: SDL3 is found with `find_package(SDL3 3.4.18 EXACT CONFIG)`, so a
distro `libsdl3-dev` of another release is ignored, and cgltf, which has no CMake package, is
accepted from vcpkg only when its header banner reads `Version: 1.15`.
M3 transport dependencies (v0.17), only with `KFM_WEBRTC` (default on; off builds without any
network dependency) and also in the headless server build: libdatachannel 0.24.6 natively (vcpkg
`libdatachannel[ws]` without default features, i.e. no `srtp` media; or FetchContent of the
release tarball plus its plog, usrsctp and libjuice submodules as separate hashed tarballs at the
commits the tag pins, built static with media off), OpenSSL as its TLS/DTLS backend (system
`libssl-dev` on the FetchContent path, the vcpkg port otherwise), and datachannel-wasm pinned to
`f9200eb` (v0.4.0 plus a missing-include fix) for Emscripten, linked with its two
`--js-library` files and `-fwasm-exceptions`. Both are exposed as one target, `kfm::datachannel`.
Content parser (v0.18): toml++ 3.4.0, header only (the include directory of the vcpkg
`tomlplusplus` port, whose compiled-library target is not linked because it fixes
`TOML_HEADER_ONLY=0` and was built with exceptions on; or FetchContent of the release tarball by
hash), in the headless server build too. `kfm::tomlplusplus` pins
`TOML_EXCEPTIONS=0` (parse errors come back as `toml::parse_result` and become diagnostics; the
loader compiles without exceptions or RTTI like the engine) and `TOML_ENABLE_FORMATTERS=0`;
only `engine/content/src/loader.cpp` includes it. TOML over JSON because tuning files need
comments. toml++ parses floats as `double`; the loader never uses that value but re-reads the
literal from the document text at the node's source position (§5.10).
On Windows the vcpkg dependencies are linked statically (triplet `x64-windows-static-md`, set by
the top-level `CMakeLists.txt` unless a triplet is given): the libdatachannel DLL's import library
exports `std::vector<std::byte>`'s members, which collide with the byte vectors every other
object file instantiates for itself (LNK2005); the static library has no dllimport side.
Audio (v0.19) adds no dependency: SDL3's audio subsystem plays everything and the sounds are
synthesised in code. On Linux SDL3 is now built with ALSA (vcpkg `sdl3[alsa]`; the FetchContent
build switches `SDL_ALSA` on when pkg-config finds `alsa`, i.e. `libasound2-dev`, and warns and
builds silent otherwise), dlopened at run time (`SDL_ALSA_SHARED`) so a machine without
libasound or a sound card just has no device; PipeWire and PulseAudio are reached through their
ALSA plugins. CI installs `libasound2-dev`. Windows (WASAPI), macOS (CoreAudio) and Emscripten
(Web Audio) need nothing.
Bot runtime (v0.30): WAMR 2.4.5 as `kfm::wamr`, FetchContent only (there is no vcpkg port) from
the release tarball by SHA256, built by `third_party/wamr/CMakeLists.txt` in its own directory
scope (WAMR's scripts use directory-scoped definitions and flags) through its
`build-scripts/runtime_lib.cmake` into a static library with SYSTEM includes and its own warning
flags; the platform comes from `CMAKE_SYSTEM_NAME` (linux, darwin, windows; anything else must
set `KFM_WASM_BOTS=OFF`) and the target from the processor (X86_64, X86_32, AARCH64, ARM; a
universal macOS build is refused). The configuration is the classic interpreter with
instruction metering and nothing else (§8), plus the two-anchor source patch that reads the
remaining budget back. C is enabled before the headless `return()` because the dedicated server
hosts modules too. Under UBSan WAMR is compiled with `-fno-sanitize=alignment` (its own UBSan
setup) and under ASan its use-after-return fake stacks are off, because they make its native
stack check report an overflow on every call. Emscripten never builds it: the browser client
instantiates modules through the browser's `WebAssembly` API behind a JS shim (an M5 follow-up).

---

## 11. Roadmap

| Milestone | Scope | Exit criterion |
|---|---|---|
| M0 Skeleton | Repo layout, CMake presets, vcpkg, CI, `core` (`Fixed`, `Vec2`, `Pool`, `BitStream`, `Rng`), Catch2, determinism harness, C++23 toolchain probe (§9) | Green CI on three OSes plus wasm running an empty replay; C++20 vs C++23 baseline decided |
| M1 Headless sim | Players, movement, dash, boomerang flight/return, catch rule, eliminations, FFA round rules, replay record/playback, scenario tests, ticks/s bench | A scripted 10-player replay runs identically everywhere |
| M2 Playable local | SDL3 + sokol_gfx client, 3D renderer with placeholder meshes (capsule monkeys, fixed camera, blob shadows, cosmetic height, GPU skinning on one rigged test model), local multiplayer, debug UI, interpolation, first feel tuning of dash and catch windows; Emscripten build of the same client runs in a tab | Two people on one keyboard/gamepads have fun for 10 minutes, natively and in the browser |
| M3 Netcode | Loopback + emulator, `NetServer`/`NetClient`, prediction + rollback, clock sync, snapshots/join, `WebRtcTransport` (libdatachannel natively, datachannel-wasm in the browser), `netsim` with 10 clients, soak tests under loss and jitter | 10 clients at 100 ms RTT, 2% loss, no desyncs, rollback depth bounded; a browser client joins a real server |
| M4 Content | Power-ups, modifiers, more abilities, arenas, modes, lobby UI, audio/VFX | Content added without touching system code |
| M5 Agent platform | `IAgent` and the versioned `Observation`, bot ABI v1 and `WasmAgent` over the wasm runtime, the reference bot in lobbies and on the server, `kfm_arena`, SDK v1 and example bots published from the bots repository (§8) | A bot built outside this repository against SDK v1 beats the reference bot in a `kfm_arena` tournament run |
| M5b Teams, modes and the grid | Teams and relations, the loadout axis, `onRoundStart`/`onPickupTaken` and `modeData`, zones, spawn groups, the tilemap arena format, `team_rounds`, `team_deathmatch`, `hill`, `capture`; the `grid` space with turns, commitments and the catcher role; fog and calls in the observation builder; `kfm_arena --team` (§5.3, §5.10, §5.12, §8) | A 5v5 bot match on a tilemap arena with `own` vision and calls replays identically across the determinism gate at one-tick turns; a human lobby plays `hill` and a 15-tick grid round |
| M6a Ship on the web | The client on Cloudflare Pages (v0.39); the Rust lobby service (room codes, connection tokens, signaling relay, server registry); the server's lobby-fed signaling and `--lobby` mode; the client's `?room=` / `--room` join flow and the `web` `IPlatformServices`; `deploy/` (images, units, Caddyfile, VM script); one VM, then 2–3 regions; Sentry (§13.5) | Send a link, friends are in a match within a minute, no install |
| M6b Play with friends | The online room: a waiting state on the server, the host's settings (arena, mode, loadout, power-ups, bots) sent over the wire and a host role that passes on; the code with copy-link and share in the client; every region listed with its availability (lobby protocol v1.1); nicknames on the scoreboard, a between-rounds scoreboard and a match-over screen with play again; a main menu and a game-looking lobby; bots yield slots to joining humans; reconnect from the UI; the lead control checked at high RTT (§6.6, §13.5, "M6b design" below) | A group of friends plays a full evening from one link without help: create, invite, pick the mode, start, play again |
| M7 The bot ladder | In this repository: the `contest` mode, the client's replay-by-URL viewer (v0.68), a released headless `kfm_arena`, the append-only JSON result. In the bots repository: accounts and teams with per-user tokens, source submissions built in a throwaway container from the pinned C++ recipe, retention of five plus the active one, the scheduler over `kfm_arena --team --rotate`, ratings per entrant, the release tag on every match and replays served with their release's content and client (v0.59), the Rust CLI on PyPI (§8 "The ladder") | A bot submitted as source through the CLI by an account outside the project is built, ranked against the reference bot and other entrants, and its match is replayed in the browser from the ladder's link |
| M8 Steam build (optional) | Steamworks `IPlatformServices`, `GnsTransport` against the Steamworks SDK, SteamPipe from CI, store page | Same game with Steam lobbies and invites; Remote Play Together for local mode |

M6b design (v0.40). A room has three states on the server, kept in the host beside
`ServerHost` as the lobby adapter is, never in `WorldState`: Waiting (the match is not running:
the server holds the initial world and ticks nothing for the sim; peers occupy slots and see the
roster, the settings and who is host), Playing (as today: a joiner spectates until the round
break in a rounds mode or spawns after `respawnTicks` in a deathmatch) and Over (the match-over
screen; the host's "play again" returns to Waiting with the same settings, a leaver's slot is
freed). The host is the room's creator, then the longest-present human when the host leaves.
Protocol version 6 adds three reliable messages: `RoomSettings` (host → server: the
`MatchConfig` axes and the bot count; the server validates against its content and
`ContentDB::compatible` and answers with the state), `RoomState` (server → everyone on every
change: state, host slot, settings, each slot's nickname) and `Start` (host → server, Waiting →
Playing through `restartMatch` with the host's config, which retires the M3 follow-up "a
server-chosen match config"); `Hello` carries a nickname of at most 16 bytes of UTF-8, kept per
slot on the server and shown on the scoreboard and the HUD. Bots yield: a human joining a room
whose human slots are full but whose bots are not takes a bot's slot at the next round break.
Reconnect from the UI is the existing token flow behind a button. Lobby protocol v1.1: the
`regions` reply gains `free` (the regions with a free server, comma-separated) beside `list`
(every registered region); a client shows the list with the availability and a create in a full
region is refused `no_server` as before. The client keeps Dear ImGui as the drawing library but
gets a game font, a palette and a main menu (Play online, Play local, Settings); copy-link uses
the clipboard natively and in the browser and the Web Share API on touch devices.
Sequencing, one PR each: (1) lobby protocol v1.1 and the availability indicator (landed in
v0.41); (2) the room
states, `RoomSettings`/`RoomState`/`Start`, nicknames (protocol 6; netcode tests over the
loopback with a scripted host; landed in v0.42, §6.6, with a functional waiting and match-over
screen that step 3 restyles); (3) the client's main menu, waiting screen, scoreboards and
match-over (landed in v0.43, §7: the game font and palette, Settings, Play online with Copy link
and Share through `IPlatformServices`); (4) bots yielding, reconnect from the UI and the lead-control check at high RTT
(landed in v0.44, §6.2, §6.6, §13.5: the queue for a bot's slot, lobby protocol v1.2's rejoin
behind the Reconnect button, the jitter-aware lead band and a 500 ms soak gate);
(5) the friends playtest against the criterion (done: M6b landed in v0.58).

M7 sequencing (v0.36, numbered M6b until v0.40): M6a and M7 share only the hosted client and
proceed in parallel. M7 depends on `kfm_arena`, static hosting and an identity provider, not on
the lobby service or the regional servers. Its order: (1) in this repository, the `contest` mode (content hash only),
the JSON result pinned append-only, a tagged release that publishes the headless `kfm_arena`
(landed in v0.60, with the versioned page per tag of v0.59);
(2) the bots repository created with the SDK moved from `tools/bots/`, the C++ layer and the
build image, `example_bot_test` kept here against the normative header (landed in v0.61); (3) the service:
accounts, teams, submissions and the sandboxed build; (4) the scheduler and ratings; (5) the
CLI on PyPI; (6) the client's replay viewer and the portal's replay links (the links landed
with step (4) in v0.65, the viewer in v0.68). M8 (Steam) is unchanged.

M7 step 3 design (v0.63). The ladder service, `service/` in the bots repository, has the shape
every hosted bot ladder converges on, sized so that growing is adding workers.

- **One process, three roles.** A Rust binary (Tokio, axum, sqlx; the lobby's toolchain) runs
  as the API and portal (`kfm-ladder serve`), as a build worker (`--worker build`) or as a match
  worker (`--worker match`). The API never builds or plays anything itself: it writes a row
  and a worker claims it. Configuration is environment variables (database URL, object store
  endpoint and keys, OAuth client ids and secrets, the session key, the engine release tag),
  never a file in the image.
- **Postgres is the system of record** from day one (SQLite's single writer is exactly what a
  job queue and concurrent workers contend on), run as a container on the API VM with a
  nightly dump to the object store; a managed database is a later swap behind the same URL.
  Tables: `users` (id, display name, created), `identities` (provider, subject, user, email
  as shown, one row per linked provider), `sessions` (portal cookies, signed and expiring),
  `tokens` (user, a SHA-256 of the secret, scope, created, revoked), `teams`, `memberships`
  (team, user, role), `submissions` (team, submitting user, created, the manifest, the source
  and module object keys, the module hash, the build log key, state: queued, building, failed,
  rejected, ready), `active_bots` (team → submission), `jobs` (kind, payload, state, attempts,
  claimed by and at, finished at, error), `matches` (seed, the release tag, the content hash,
  the two submission ids and sides, the replay key, the parsed result, created), `ratings`
  (team, mu, phi, sigma, games, updated) and `seasons` (name, start, end, the `contest` row's
  name). Migrations live in the repository and run at start.
- **Blobs go to S3-compatible object storage**, Cloudflare R2 first (the page is already on
  Cloudflare, no egress fee for replays served to browsers), behind the S3 API so MinIO serves
  tests and development and any other store a move; keys are
  `sources/<submission>.zip`, `modules/<submission>.wasm`, `logs/<submission>.txt`,
  `replays/<match>.kfmr`. Never in Postgres, never on a VM disk that needs backing up.
- **The jobs table is the queue.** A worker claims with `UPDATE … WHERE id = (SELECT … FOR
  UPDATE SKIP LOCKED)`, heartbeats while it runs, and a job whose claim goes stale is
  re-queued up to a small attempt count; there is no broker to run. Kinds: `build`
  (submission → module, or a failure with its log), `validate` (the module through the checks
  `WasmAgent` makes plus one smoke match against the reference bot with the season's
  `kfm_arena`; a trap or an over-budget tick rejects it), `match` (one `kfm_arena --mode
  contest --team A --team B --rotate --json --replay-dir` run, its JSON parsed per
  `docs/arena-result.md`, the replay uploaded, the ratings updated in one transaction).
- **Identity.** OAuth with GitHub or Google (authorization code with PKCE; the `openidconnect`
  and `oauth2` crates), the account keyed by the provider's subject id; a signed-in user may
  link the other provider from the portal, and an email is display data only. A portal
  session is a signed, expiring cookie. An API token is 256 random bits shown once, stored as
  its SHA-256, scoped `submit` (upload, activate, read own) or `read`, sent as a bearer; the
  CLI stores it after `login`, which is the browser flow with a local redirect. A team's
  membership table means a member leaves without rotating anything.
- **The HTTP surface** (`docs/ladder-api.md` in the bots repository is normative, JSON in and
  out, append-only like the engine's contracts): `/healthz`; `/auth/{github,google}` and their
  callbacks; `/auth/logout`; `GET /me`; `/tokens` (list, create, revoke); `/teams` (create,
  invite by user, leave, list mine); `POST /teams/{t}/submissions` (multipart: `manifest.toml`
  and `source.zip`, at most 4 MiB of source, the manifest names the language and the entry
  directory), `GET /teams/{t}/submissions`, `GET /submissions/{s}` (state, hash, log URL),
  `POST /teams/{t}/active` (set the active submission); `GET /ladder` (ratings, paged);
  `GET /matches?team=` and `GET /matches/{m}` (the result, the replay URL, the viewer URL
  `https://kfm.minip8.com/<tag>/?replay=<url>`, the client feature of step 6, landed in v0.68). The portal is the
  same routes rendered as HTML by the same binary (askama templates, no frontend framework):
  sign in, tokens, teams, submissions with their logs, the ladder, a match's result.
- **Retention.** Five submissions per team plus the active one; pruning deletes the source
  and log objects, never a module a recorded match used, so every ranked match stays
  reproducible from seed plus modules for the season.
- **Scheduling and ratings.** A scheduler tick (every few minutes) makes one round: every
  team with an active, validated submission is paired with up to k opponents near its rating
  (Swiss-style, k = 3 to start, never the same pair twice in a round), each pair is one `match`
  job played both sides through `--rotate`, so match volume is linear in entrants. The rating
  is Glicko-2 per team over the two-sided results, updated as each match lands; at an engine
  release the deviation is widened (v0.59), never reset. `seasons` freezes the ladder at its end
  date; a new season starts ratings afresh.
- **Deployment.** Two VMs, as `deploy/` does for the lobby: `kfm-ladder-1` runs Caddy, the API
  and portal, Postgres and the match workers (a match is a `kfm_arena` process, already the
  sandbox); `kfm-build-1` runs only the build worker, with Docker and gVisor (`runsc`) as the
  runtime of the §8 command, no inbound ports, and the §8 caps. Images from GHCR on the bots
  repository's tags, systemd units, Sentry. The engine release the season runs on is the one
  baked into the service image (v0.64; the design said fetched on the workers at start): the
  image's entrypoint pins `KFM_LADDER_ENGINE_TAG` to the baked runner, and a season moves
  engines by bumping the bots repository's `engine.lock` and releasing (v0.71: the engine's
  release workflow opens that bump as a pull request, `tools/bump_engine.sh` there; the merge
  and the bots release stay a person's).
- **Deferred, on purpose.** Kubernetes, a message broker, microservices, a separate frontend,
  rate limiting beyond a per-token submission cap (one build per team per minute), and
  email. The contest's season rules (prizes, eligibility) are the portal's text, not code.

Sequencing, one PR each in the bots repository: (3a) the service core: the crate, config,
migrations, `/healthz`, OAuth for both providers, sessions, tokens, teams, `docs/ladder-api.md`,
tests over a Postgres in CI; (3b) submissions and the build path: the upload endpoint, the
object store with a MinIO-backed test, the jobs table and worker loop, the build worker over
the sandbox image, validation with the released `kfm_arena`, activation and retention; (3c)
deployment: images, units, the gVisor runtime, the two VMs scripted, the runbook. All three
landed in v0.64 (bots PRs #3 and #4). Steps (4) scheduling and ratings, (5) the CLI and (6) the
viewer follow as in the M7 sequencing.

Contest design: zones to capture and keep (v0.79; designed, not built). The contest as
shipped (king of the hill on `courtyard_hill`, v0.69) has three faults, each measured with
`kfm_arena` in the ladder's shape (5v5, `--rotate`; the v0.79 changelog entry has every number).
**It stalls**: two teams that both stand on the hill hold every clock, and 156 games of 200
between two copies of the house bot `holder` ran to the 3600-turn clock (48 draws). **Standing
beats fighting**: `holder` is the starter with one change, staying on the hill unarmed, and it
beats the starter 187 to 13 while being eliminated one and a half times as often as it
eliminates. **A banana thrown from range does not land**: of the banana hits in 100-game runs,
1 to 5 % came from a flight of 3 to 5 turns and 53 to 92 % from one of at most 1 turn (the rest
are ricochets and returns), which is §12's open grid item seen from the other side. The season
has no entrant from outside the project yet, so the rules change now rather than later.
Decided:

- **A new kind, `control`** (`ModeKind::Control`, the ModeBehavior table of §5.3: a behaviour
  file, a loader name, a registry line, a scenario test; `hill` stays as it is for human
  rooms). It is hill's round, clock, respawns and roster over **every zone of the arena**. A
  zone has an owner, nobody at the start. Each Playing tick, per zone, the monkeys in it
  (`zoneOccupants`) decide: the team with **strictly the most** monkeys there, when it is not
  the owner, is the zone's challenger and gains a tick of capture progress (a new challenger
  starts from zero), and at `capture_ticks` it owns the zone (`ZoneCaptured{a = team, b =
  zone}`); the owner with the most, or an empty zone, clears the progress; a tie for the most
  holds it. An owned zone pays its team's clock one tick per Playing tick **whether or not
  anyone stands on it**, and every `hill_ticks` of clock score a point (to the team's lowest
  present slot, the remainder carried). First team to `score_to_win`, else the leader at
  `time_limit_ticks`, a tied lead a draw; eliminations score nothing. Majority, not sole
  occupancy, because a prototype of majority on today's hill took the stall from 80 games of
  100 at the clock to 20, and to none with the respawn below; capture and keep, not presence,
  so that a team must leave a zone to win and five monkeys cannot cover three.
- **Its state is `modeData`, as every objective kind's** (`.claude/rules/sim.md`): words 0-1
  the clocks of teams 0 to 3 (u16 each, hill's layout), word 2 the owners and word 3 the
  challengers (4 bits per zone, the team + 1, 0 nobody), words 4-7 the capture progress (u16
  per zone). So the kind plays at most four teams (`kMaxControlTeams`, a loader error past
  it) and eight zones (`kMaxArenaZones`), and bots read all of it in
  `match.reservedModeData` words 0-7 with no layout change. One new `ModeDef` field,
  `captureTicks` (`capture_ticks`, at least 1); `hill_ticks` is reused as the zone-ticks per
  point. `ContentDB::compatible`: an arena with a zone. The scripted generator steers to the
  nearest zone its team does not own, Rng-free and keyed on the kind, and a corpus entry
  plays it.
- **Three zones, on a new arena** (appended; `courtyard_hill` stays for `hill`). The draft is
  the courtyard with a home zone at each team's door and the old hill between the pillars,
  still symmetric under a half turn (zone 1 maps to zone 2); it is redrawn if the measurements
  below say so:

  ```
  "...........~~...........",
  "...........~~...........",
  ".0...##..........##...8.",
  ".....##..........##.....",
  "........................",
  ".1.......#....#.......7.",
  "....bbb..#aaaa#..ccc9...",
  "...4bbb..#aaaa#..ccc....",
  ".2.......#....#.......6.",
  "........................",
  ".....##..........##.....",
  ".3...##..........##...5.",
  "...........~~...........",
  "...........~~...........",
  ```

  Walking, a team reaches its home zone in 3 turns, the centre in about 10 (round a pillar)
  and the enemy's home in about 17, so a home zone is cheap to take and costs a raid to lose,
  and the centre is where the points are: with one zone a team scores a point per
  `hill_ticks`, with two twice as fast.
- **`Observation` version 2, appended** (§8, `docs/bot-abi.md`): version 1 shows zones 0 and
  1 in `reservedModeData` words 8-15 and has no room for a third. Version 2 is version 1 plus
  a `zones` block at the end (`count`, then eight closed boxes in Q16.16: 132 bytes, 5880 in
  all); nothing of version 1 moves, the words 8-15 stay filled, and fog never hides a zone.
  The host speaks both: a module whose `kfm_abi_version()` is 1 is written version 1's 5748
  bytes under a version 1 header, so the fixtures, the examples as built and any module on the
  ladder keep running unchanged, and only a bot that wants the third zone is rebuilt. Mode
  kind 7 is `control` (`KFM_MODE_CONTROL`); the SDK header names the `modeData` words. A third
  version is still a refusal.
- **Bananas that land from range, by flight and by fog, not by size.** Three content knobs
  tried on today's row moved nothing (hearing 3 to 1 or 0, the banana 2.3 times wider, 2 or 4
  ticks of input delay: the 3-to-5-turn share stayed 4 to 8 %), partly because today's bots
  only throw from 3 cells. The cause is the flight: 1.07 cells a turn against a walk of 1,
  heard 3 cells out whatever the facing. So (a) the projectile `speed_mul` gets its own limit,
  8 (the 4 stays for radius; moves and contacts are swept, nothing tunnels, §5.6), and the
  contest's baseline flies the banana at 7.5, **2 cells a turn**, which is the 15-tick grid
  mode's speed; (b) a mode may override the banana's flight, `banana_outbound_ticks` and
  `banana_turn_rate` (0: the `BoomerangDef`'s), read by ProjectileSystem and reported in the
  observation's rules block, because at 2 cells a turn the def's 36 outbound ticks are 72
  cells of bouncing and its 6.6 degrees a tick cannot turn the banana home on a 24 x 14
  arena: the contest gets 3 turns out (6 cells, the range §12 wanted) and a return that
  turns in a few cells. These are `ModeDef` fields, not `Boomerang` fields, so the world
  state and every existing checksum stay as they are; (c) `vision.hearing` goes from 3 to 1.
  A banana in the cone (4 cells, 5 at rest) is then seen two turns out and side-stepped, and
  one from outside the cone is heard a cell away, which at 2 cells a turn is too late: where
  a monkey looks decides what it can dodge. This is §12's "per-mode `BoomerangDef` override"
  taken up, and it is measured before the row ships (the gates below).
- **Power-ups early and often, in turn-sized twins.** Today the arena lays `quick_hands`
  alone, first at turn 600 of a game that lasts about 650, for 600 turns. The contest arena's
  spawners (the two on the centre line, away from every zone, so a pickup costs a monkey its
  place) lay from about turn 90 and every 240, one alive each, from `big_banana`,
  `triple_banana`, `quick_hands` and `shield`, each as a `_1t` twin of 60 to 90 turns (a mode's
  numbers that scale with the turn need their own defs, as the kits have). `speed_boost` is
  left out: a walk is a cell a turn on the grid whatever the speed.
- **The row.** `contest` becomes `kind = "control"` on the new arena with a `contest_base`
  baseline (`grid_base_1t` is `grid_rounds_1t`'s and stays), and starts from: `capture_ticks`
  30, `hill_ticks` 60, `score_to_win` 10, `time_limit_ticks` 3600, `respawn_ticks` 240 and
  `respawn_invuln_ticks` 8 (on today's hill 240 and 8 took the mirror's stall from 80 of 100
  to 14, and 360 made the first won fight the game: a median of 414 turns against a
  walkover's 371), the regrow, roles and calls unchanged. Every one of these is content and
  is tuned against the gates, with control-aware `starter` and `holder` builds: over 200
  games, at most 5 % at the clock and 5 % drawn; a median game of 400 to 900 turns; at least
  a quarter of banana hits from a flight of 3 cells or more; no fault and `fuel_max` under
  80 k; and the side split explained (below).
- **Not changed.** The roles (one catcher, four standard): a role is a loadout, three ability
  slots, so roles that differ in hp or size need an engine change and wait. The reference
  bot: it ignores zones as it ignores the hill, plays human rooms as it does today, and stays
  the ladder's validation opponent, which judges faults, not strength. The ladder service:
  it points at the row by name (v0.59).

Order, one PR each, the first three here: (1) the `control` kind, its field, loader, scenario
tests and corpus entry (the content hash moves, so the corpus is re-recorded; no existing
checksum may change); (2) `Observation` version 2, the host's two versions, the SDK header and
`docs/bot-abi.md`; (3) the banana's flight (the limit, the mode's override), the power-up
twins, the arena, the row, the client (every zone tinted by its owner, the HUD's and the
scoreboard's cases for the kind), the measurements and a release; (4) in the bots repository
the pin bump, the C++ layer's zones and control state, `starter` and `holder` for the kind, the
prose, a release, and the house teams resubmitted. Open, to settle in (3) by measurement:
whether a tie should decay the capture progress instead of holding it; a tie-break at the clock
(zones owned, then the clocks) before a draw; whether the home zones should start owned; and
**the side bias**: in every sole-control run of `holder` against `starter` the east side won 55
to 77 games of 100 whichever bot stood there, and a mirror of `holder`, whose goal is the same
under the arena's half turn, split 50 to 102. `--rotate` cancels it per entrant, but it halves
what a pair of games says, and its cause (the bots' scan orders, or the engine's slot-order
tie-breaks) is not isolated: a scenario test that mirrored inputs give mirrored outcomes comes
first, because every gate above is read through it.

M6a sequencing (v0.37), one PR per step: (1) the lobby service and its protocol, with a Rust
test suite over an in-process client and a fake server; (2) the server's lobby-fed signaling
(the `WebRtcTransport` signaling source, `ServerHost --lobby`), checked by `kfm_net_probe`
and a native `kfm_client` through a local lobby; (3) the client's join flow (`?room=` and
`--room`, the create and join UI, the `web` `IPlatformServices`), verified by
`check_browser.py` against a local lobby and server; (4) `deploy/`, the Pages workflow, one
EU VM and a playtest with friends over the real link (the open M3 follow-up); (5) more
regions and Sentry. Cheap follow-ups fold in where they fall: bots that yield a slot to a
joining human, the warm-up round a joiner lands in, reconnect-with-token from the UI.

M5b sequencing (v0.29): one PR and one architecture version per step, each green on the
determinism gate before the next starts. (1) Mode foundations: `kMaxModes` 32, the loadout axis,
`onRoundStart`/`onPickupTaken`, `modeData` (content hash changes once; `modeData` regenerates
the corpus). (2) Teams: `MatchConfig::teams`, `relation`, the relation mask and row 31, spawn
groups, `team_rounds` and `team_deathmatch` (checksums unchanged). (3) Objectives: zones, the
tilemap format, `hill`, then `capture` (checksums unchanged). (4) The `grid` space: the
`SpaceBehavior` table, commitments, rooting, the radius stat, the catcher loadout, a grid
rounds mode (the commitment fields regenerate the corpus). (5) The squad contest: `vision`,
calls, `Observation` fields, `kfm_arena --team`, per-instance lifecycle (no sim checksum
change beyond the `call` ability's event). Steps 1 to 3 are worth doing even if the grid is
dropped; step 4 is the only one with design risk and is prototyped behind the table before the
contest layer goes on top.

Progress: M7 step (1) landed in v0.60: the `contest` row (grid_rounds_1t renamed, pinned equal by
a loader test, with `scripted_10p_contest_1800t` in the gate), `docs/arena-result.md` and the
frozen-key test with `content_hash`/`build_hash` appended to the match object, and the release
workflow's `release`, `arena` and `arena-sums` jobs plus the web job's site asset and versioned
paths (`deploy/README.md`). Verified locally: the headless and `dev` builds with every non-soak
test, the corpus regenerated with every existing `.checksums` byte-identical, `kfm_arena --mode
contest --team ... --rotate` ending its matches on courtyard, a fully static Linux `kfm_arena`
running the echo module inside a Debian trixie image, and the workflow's shell steps run under
bash against a fake `gh`. Not yet run: the workflow itself (no release exists yet; the first
tag creates one), the macOS and Windows legs, Cloudflare's `:version` rules. Open: a stamped
build hash; a global concurrency group for `web` when two tags land close together; a
`CMAKE_OSX_DEPLOYMENT_TARGET` for older macOS.

Progress: M7 step (2) landed in v0.61 in `minip8/kung-fu-monkeys-bots` (the SDK, the C++
layer, the examples and starter, the recipe, the image, the engine pin and CI; the v0.61
changelog line has the measurements). Here only `tools/bots/README.md` points at it and the
stale `chaser.wasm` was rebuilt. Not yet run: the bots repository's CI on GitHub (its
`fetch_kfm_arena.sh` needs the first engine release; until then CI builds the runner from the
pinned commit). Next: step (3), the service (accounts, teams, submissions, the sandboxed build),
which needs a hosting decision for the service and its build host.

Progress: M7 step (3) landed in v0.64 in the bots repository (`service/`, `deploy/`, bots PRs #3
and #4; the v0.64 changelog line has the departures and measurements). Not yet deployed: the two
VMs, the R2 bucket, the GitHub and Google OAuth apps.

Progress: M7 step (4) landed in v0.65 in the bots repository (bots PR #5; the v0.65 changelog
line has the decisions and measurements).

Progress: M7 step (5) landed in v0.66 in the bots repository (bots PRs #6 and #7), and the
ladder was deployed in v0.67 (`bots.kfm.minip8.com`, `kfm-cli` on PyPI, the first season; the
v0.67 changelog line has the shape as built). Next: step (6), the client's replay viewer, here,
and an entrant from outside the project through the whole path.

Progress: M7 step (6) landed in v0.68 here (`ReplaySession`, `ReplayControls` and the client's
`--replay` / `?replay=` screen, §7 "Replay viewer"; the v0.68 changelog line has the tests and
what was verified). The portal's links already point at it (bots repository, v0.65). What
remains of the M7 criterion: a tagged engine release, because the ladder's
`/<tag>/?replay=` link reaches the page of the match's tag, and matches recorded under v0.5.1
link to a page without the viewer, by design (v0.59: the viewer is the release's client), so
the first replay watchable from the ladder is one played after the ladder runs on a release
that carries the viewer (a pin bump of the bots repository's `engine.lock` and a bots release,
v0.64); and a bot submitted by an account outside the project through the whole path, from
`kfm submit` to its replay in the browser. Not yet run: CI's real xvfb for the two new
`client_smoke_replay*` tests.

Progress: M6a's code landed in v0.38 (steps 1 to 3 of the v0.37 sequencing and the files of
step 4): the lobby protocol and service, the transport's lobby modes, `LobbyLink` and
`kfm_server --lobby`, the client's room flow natively and in the browser, `deploy/` and the
release workflow. Verified locally: the Rust suite, the headless and `dev` builds with every
non-soak test, the real-socket lobby tests, a native lobby + server + two clients on one
machine, and the browser page creating a room through a local lobby in headless Chromium
(§13.5 as built). Then a test deployment on Google Cloud (`kfm-asia-1`, an `e2-small` in
asia-south1 created by `deploy/gcp/create_vm.sh`, the images loaded from a local build, a
`<ip>.sslip.io` name for Caddy's certificate): `/healthz` over HTTPS, two native clients from a
home connection in one room on that server at 160 ms RTT for 30 s with 0 checksum mismatches
and 0 resyncs, and the browser page creating and joining a room from headless Chromium. Two
things that test taught: a client's `--exit-after-frames` counts frames and a headless client
without vsync runs hundreds of frames a second, so a real-link check needs a budget of
thousands (the first runs ended before the handshake and looked like a network fault); and a
room holds its server process for the ten-minute expiry after its last peer leaves, so a pool
of N processes serves at most N rooms created in any ten minutes, abandoned ones included
(size the pool for that, or shorten `--room-ttl-seconds`). Open: `late_inputs/s` on the server
was high at 160 ms RTT while substitutions stayed near 1/s, so the lead control deserves a
look in the human playtest. Not yet done: the production names and secrets, the GitHub Pages
site, a tagged release, the playtest with friends over the real link, the second region and a
Sentry project; the exit criterion ("send a link, friends are in a match within a minute") is
judged there. Deployed (v0.39): the production VM `kfm-au-1` (an `e2-small` in
australia-southeast1, four server processes, the `v0.1.0` images from GHCR, region `au`), the
lobby at `wss://lobby.kfm.minip8.com` behind Caddy with a Let's Encrypt certificate, and the
page at `https://kfm.minip8.com/` from Cloudflare Pages. Verified from a home connection: a
native client in a room on that server at 34 ms RTT for 40 s with 0 checksum mismatches, and
the published page creating a room in headless Chromium with 0 console errors. The
playtest with friends over the link was played (v0.58, with M6b); a second region and a Sentry
project remain.

Progress: M5b landed in v0.35. Against the exit criterion: "a 5v5 bot match on a tilemap arena
with `own` vision and calls replays identically across the determinism gate at one-tick
turns" is verified: `kfm_arena --mode grid_rounds_1t --team ref:hard,ref:hard,ref:hard,
ref:hard,ref:hard --team ref:hard,ref:hard,ref:hard,ref:hard,ref:hard --matches 3
--replay-dir DIR` on `courtyard` under `own` fog with calls (894 calls in the three) ends all three (1288, 1358 and 1321 ticks), and each `.kfmr`, recorded by
the Release build, replays to the result line's final checksum in `kfm_replay checksums` and
passes `kfm_replay verify` in the Debug build; the gate itself carries the shape as
`scripted_10p_grid_rounds_1t_1800t` (ten scripted monkeys in two teams on the courtyard at
one-tick turns, the grid kits' calls pressed), beside the two 4-player grid entries, on every
CI leg. "A human lobby plays `hill` and a 15-tick grid round" is verified as far as a machine
can: `kfm_client --no-settings --autostart --bots 3 --mode hill` and `--mode grid_rounds`
render their matches through the countdown into play (screenshots, the F1 overlay with its
Calls section); the human playtest itself is pending, as for M2.

Progress: M5b step 5 (the squad contest) landed in v0.35. The `call` ability kind with
`range` and `MonkeyCalled` (§5.5, §5.9); `ModeDef::vision` and `compatible()`'s cells for fog
(§5.10); fog in `buildObservation`, the turn window, calls and the per-instance lifecycle in
`AgentHost`, the filled Observation v1 fields, `kfm_arena --team`, `--memory-on-respawn`, the C
SDK's names and the grid- and fog-aware reference bot (§8); the client's shout, ping and Calls
overlay (§7). Decisions recorded on the way: the call goes in the grid kits' third slot (the
only free one), so the two grid corpus entries change once; `range` is Chebyshev between the
centres in units; a fog kind needs cells; focus means at rest and not moved on the previous
tick; pickups follow the cell visibility; an unseen monkey keeps its roster fields; the window
closes after each latch observation and calls appear from the next tick to the next latch; only
the own cell is reported (`ObsPlayer` has no cell field); a slot is paused only once its monkey
existed, so first frames do not change; `kfm_arena`'s team split follows the hosts' round
robin. Commits in order: the call kind (checksums unchanged); the vision block (content hash
only, every `.checksums` byte-identical); the grid content (the two grid entries' checksums
changed, corpus regenerated; the 5v5 entry new, 21 replays); fog, calls and the lifecycle;
`kfm_arena --team`; `kfm_server --memory-on-respawn`; the reference bot; the presentation; the
SDK names; a Release-only `-Wnull-dereference` fix in the capture scenario; the bot's pit
avoidance; docs; lint. Replay checksums: unchanged except `scripted_4p_grid_rounds_3600t` and
`scripted_4p_grid_rounds_1t_1800t` (the call slot), regenerated. Netcode: `kfm_netsim
--clients 6 --mode grid_rounds_1t --rtt 100 --jitter 10 --loss 2 --seconds 20`: 701
checksums compared, 0 mismatches, rollback depth max 11 (p99 9), the scripted inputs pressing
calls. Bench (`kfm_replay bench --ticks 36000 --players 10`, Release, `step()` alone, the
shared WSL2 machine): 1.26 µs/tick on `dojo`/`rounds` (unchanged), 2.03 µs on
`courtyard`/`grid_rounds` and 2.31 µs on `grid_rounds_1t` (calls cost nothing measurable). Bot
matches (Release): `kfm_arena --mode grid_rounds_1t --bots 4 --matches 5` 5 of 5 ended (from 5
capped in v0.34), `--mode grid_rounds --matches 3` 3 of 3 (from 3 capped), 20 rotated 5v5
matches on `grid_rounds_1t` 20 ended, 10 wins a side, about 30 k ticks/s with ten fogged bots.
Verified locally on GCC 15: the headless Debug build, the dev preset (all 45 ctest targets
incl. the 21-entry corpus, the new `arena_smoke_team`, both soak gates and the real-socket
tests), the Release build and its 39 tests, clang-format 23.1.3 and clang-tidy 23.1.4 over the
whole tree with zero findings, the no-float check (fog included), `build_example.sh` with the
emsdk clang (byte-identical modules), and Emscripten 6 (the `wasm` preset: build and its 39
tests under node); MSVC, Apple Clang, Clang and ASan+UBSan are left to CI. Follow-ups: the call
alphabet mask and a per-mode call range check against the
vision; zones and the other monkeys' cells in a later Observation version; a reference bot
that plans for its team, reads calls beyond the last sighting and takes the objective; the
human playtest of `hill` and a 15-tick grid round; the §12 grid numbers.

Progress: M5b step 1 (mode foundations) landed in v0.31. `kMaxModes` is 32; loadouts are a
match axis (`[[loadout]]` in `content/loadouts.toml`, loadout 0 `base` = the `[player]
abilities`, `MatchConfig::loadout` through `MatchContent`, replay format 4, protocol 4, the
lobby's Loadout row and `loadout=` setting, `--loadout` on `kfm_client`, `kfm_server`,
`kfm_netsim`, `kfm_render_probe`, `kfm_replay` and `kfm_arena`, `?loadout=`), and the
`rounds_blink`/`rounds_slam`/`rounds_parry` modes became the `blink`/`slam`/`parry` loadouts;
`ModeBehavior` has `onRoundStart` and `onPickupTaken` (no-ops in the three shipped kinds) and
`MatchRules::modeData` holds eight mode-owned words (§5.3, §5.10). Observation v1 fills
`self.reservedRole` and `match.reservedModeData` without a layout change (§8, `docs/bot-abi.md`
§3). Replay checksums: the loadout axis and the larger mode table changed only the content hash
(corpus re-recorded, every `.checksums` byte-identical, the three ability replays included now
that they record `rounds` plus a loadout); `modeData` then changed every world checksum (corpus
regenerated; the gate passed against the old checksums with `modeData` left out of the visitor,
so play is unchanged). Verified locally on GCC 15: the headless Debug build, the dev preset
(every ctest target incl. the 14-entry corpus, both soak gates and the real-socket tests), the
Release build (corpus, bench), clang-format 23.1.3 and clang-tidy 23.1.4 over the whole tree
with zero findings, the no-float check, `kfm_client --loadout slam --bots 3 --autostart` 60
frames with a screenshot and the lobby with its Loadout row, `kfm_netsim` 6 clients at 100 ms /
2 % on `--loadout slam` (0 mismatches) and `kfm_arena --loadout parry`; MSVC, Apple Clang,
Clang, ASan+UBSan and Emscripten are left to CI. Bench (`kfm_replay bench --ticks 36000
--players 10`, Release, `step()` alone, the shared WSL2 machine): 1.30–1.38 µs/tick on
`dojo`/`rounds`, 1.40 µs/tick with `--loadout slam`, within the noise of v0.25's 1.23 µs.
Follow-ups (later M5b steps): the per-role loadout lookup and `MatchConfig::teams` (step 2), the
first kinds that use the new hooks (`hill`, `capture`, step 3), and `--powerups` on
`kfm_netsim` and the render probe (open since M4; both now take `--loadout`).

Progress: M5b step 2 (teams) landed in v0.32. `MatchConfig::teams` through `MatchContent`
(`relation`, `teamScore`, `loadoutIndexFor`), replay format 5 and protocol 5, filled by every
host from the mode (§5.4, §6.6); `InteractionRule::relationB` with seven Ally twins behind
`Tag::FriendlyFire` (§5.6); `ModeDef::teams`, `baseline` (a permanent modifier, §5.8) and
`roles`; `ArenaDef::spawnGroups` and `ContentDB::compatible` (§5.10); the kinds `team_rounds`
and `team_deathmatch` and the `standoff` arena (§5.3); the lobby's read-only Teams row, team
tags on the lobby cards and the scoreboard, team scores under it and "Team N" banners (§7);
Observation v1's team fields and a reference bot that ignores a teammate's banana (§8).
Decisions recorded on the way: twins for the parry, slam-grounding and shield rows too, with
the tag on the receiving monkey (§5.6); the baseline as a permanent `ModifierList` entry applied
in full at the spawn, durations capped at 65534 (§5.8); `team_rounds` credits a round to the
winning team's lowest survivor and `team_deathmatch` keeps a leaver's score and scores no ally
kill (§5.3); `dojo` keeps one spawn group and the team modes play on a new arena, because two
groups would move free-for-all slots and change the dojo's checksums (§5.10); a command line
with no arena takes the first compatible one, a named incompatible arena is refused, and the
client's saved arena gives way to a mode flag (§5.10); no friendly-fire power-up ships, since a
new power-up would join the `dojo_powerups` spawner draws (§5.8). Replay checksums: unchanged.
The content hash (new `ModeDef`/`ArenaDef` fields, two modes, one arena) and the replay format
changed, so the corpus was re-recorded and every existing `.checksums` file is byte-identical;
two entries are new, `scripted_4p_team_rounds_1800t` and `scripted_4p_team_deathmatch_3600t` on
`standoff` (16 replays). Spawn groups and the §12 slot bias, 4 hard reference bots, fixed
slots, `kfm_arena` Release: `dojo`/`rounds` over 200 matches wins 28/77/64/31 by slot, the
known bias, untouched (`dojo` keeps one group); `standoff`/`rounds` 68/84/19/17 (12 capped):
in a free-for-all the groups alternate by slot, which trades the arc bias for a
same-side one, so spawn groups do not fix free-for-all bias and `standoff` is a team arena;
`standoff`/`team_rounds` over 200 matches the west team (slots 0, 2) won 94 and the east team
(1, 3) 100 (6 capped), and 3v3 over 100 matches 56 to 43 (1 capped): no side bias between
teams within the noise. Verified locally on GCC 15: the headless Debug build, the dev preset
(every ctest target incl. the 16-entry corpus, both soak gates and the real-socket tests), the
Release build, clang-format 23.1.3 and clang-tidy 23.1.4 over the whole tree with zero
findings, the no-float check, `kfm_client --no-settings --autostart --bots 3 --mode team_rounds`
120 frames with a screenshot (two teams spawned on their sides, team bars on the cards, team
scores) and the lobby with its Teams row, `kfm_netsim --clients 6 --mode team_deathmatch` at
100 ms ± 10 / 2 % for 20 s (701 checksums compared, 0 mismatches) and `kfm_arena --bots 4
--mode team_rounds --matches 3`; MSVC, Apple Clang, Clang, ASan+UBSan and Emscripten are left
to CI. Bench (`kfm_replay bench --ticks 36000 --players 10`, Release, `step()` alone, the
shared WSL2 machine): 1.23–1.25 µs/tick on `dojo`/`rounds` (the relation check costs nothing
measurable), 1.21 µs on `standoff`/`rounds`, 1.86 µs on `standoff`/`team_rounds`, 2.55 µs on
`standoff`/`team_deathmatch` against 2.20 µs for `deathmatch` on the same arena (teammates'
bananas fly through instead of ending in a hit, so more stay in the air). Follow-ups: a
non-spawnable power-up flag so a friendly-fire baseline can ship; the reference bot keeping its
throws off a teammate's line under friendly fire; `kfm_arena` places and result lines per team
(with `--team`, step 5); a fairer free-for-all spawn (a per-round shuffle of slots onto ranks,
§12) for arenas with several groups.

Progress: M5b step 4 (the grid space) landed in v0.34. The `SpaceBehavior` table with the
continuous and grid rows (§5.3, §5.12), `ModeDef::space`/`turnTicks`/`dashCells`/`arena`,
`Player::moveTarget`/`moveTicks`, the radius stat (§5.8, §5.10), `AbilityDef::rootPhases`
(§5.10), the 0-tick and full-phase perfect windows (§5.5), `compatible()`'s cells, the public
cell mapping, the grid content (`grid_rounds`, `grid_rounds_1t`, four loadouts, `quick_hands`,
two baselines, `courtyard`), the presentation of the radius stat and the lobby's own-arena and
baseline rows (§7). Commits in order: the refactor behind the table (gate unchanged); the
commitment fields and `Stats::radius` in the world state (every `.checksums` changed once,
corpus regenerated; with the fields out of the visitor the gate passed on the old checksums);
the radius reads; rooting; the grid space; the content; presentation; the lobby; lint. Every
other commit kept every `.checksums` byte-identical (the content hash changed three times, the
`.kfmr` were re-recorded). Two corpus entries are new, `scripted_4p_grid_rounds_3600t` and
`scripted_4p_grid_rounds_1t_1800t` on `courtyard` (20 replays); the scripted generator needed no
grid awareness (its stick and buttons quantise at the latch: in `kfm_replay stats --players 4
--matches 10`, `grid_rounds` plays 33.3 s rounds with 54 throws, 4.7 hits, 2.6 eliminations and
5.0 catches by the catcher's dash per round, 8 of 10 matches finished inside the 10-minute cap;
`grid_rounds_1t` 4.4 s rounds, all finished). Reference bots (hard, unchanged, no grid
awareness): `kfm_arena --bots 4 --mode grid_rounds_1t --matches 5` reaches the 36000-tick cap in
all five with 6 eliminations in total, and `--mode grid_rounds --matches 3` all three with 6:
the bot sees every banana and side-steps a cell faster than a 64 u/s (or 8 u/s) banana closes,
so grid play needs bots that read the grid (step 5) and the open numbers below. Netcode:
`kfm_netsim --clients 6 --mode grid_rounds --rtt 100 --jitter 10 --loss 2 --seconds 20` and the
same for `grid_rounds_1t`: 701 checksums compared, 0 mismatches, rollback depth max 11 (p99 9).
Bench (`kfm_replay bench --ticks 36000 --players 10`, Release, `step()` alone, the shared WSL2
machine): 1.26 µs/tick on `dojo`/`rounds` (unchanged), 1.90 µs on `courtyard`/`grid_rounds` and
2.33 µs on `courtyard`/`grid_rounds_1t`, against 2.28 µs for `team_rounds` on the same arena:
the latch and the slide cost less than the stick's wall sliding. Verified locally on GCC 15:
the headless Debug build and the dev preset (every ctest target incl. the 20-entry corpus, both
soak gates and the real-socket tests), clang-format 23.1.3 and clang-tidy 23.1.4 over the whole
tree with zero findings, the no-float check, `kfm_client --no-settings --autostart --bots 3
--mode grid_rounds` (the courtyard, two teams, monkeys hopping cell to cell, the F1 overlay);
MSVC, Apple Clang, Clang, ASan+UBSan and Emscripten are left to CI. Follow-ups: grid-aware bots
and the observation's cell, facing octant, turn clock and `ticksToLatch` (step 5); a per-mode
banana flight (or flight in turns) so the 1-tick variant can hold the 15-tick numbers; the
Blink/Slam startup rooting; a non-spawnable power-up flag in place of the explicit spawner sets.

Progress: M5b step 3 (objectives) landed in v0.33. Arena zones (`ArenaDef::zones`,
`zoneOccupants()`) and the tilemap arena format (`cell`, `map`, merged into the same wall and
hazard boxes, `cellSize` recorded) in `engine/content` (§5.10); `ContentDB::compatible` with
hill's zone and capture's zone per team, `kMaxArenas` 16; the kinds `hill` and `capture`
(§5.3), `PickupKind::Flag`, `Tag::Carrying`, rule row 52 (§5.6), `HillHolderChanged` (§5.9);
the tilemap arenas `hilltop` and `outposts`, the modes `hill` and `capture`; zones, flags and
carriers in the client (§7); the flag, the kinds and Carrying named in Observation v1 (§8,
`docs/bot-abi.md`). Decisions recorded on the way: both kinds keep their state in `modeData`
and add no world-state field, the flag's team riding in the pickup's `spawner` byte (§5.3); a
hill point goes to the team's lowest slot standing in the zone (§5.3); a flag's own team cannot
touch it, so only the clock returns a dropped flag, and a flag dropped on tick T is home on
T + `flagReturnTicks` (§5.3); tilemap hazards list pits before mud, and a cap overflow is an
error that names the merged count (§5.10); `outposts` is also compatible with `hill`, content
decides which zone is fair (§5.10); `kMaxArenas` rose to 16 because the two arenas filled the
8 (§5.10); the scripted generator walks to the objective in these two kinds only, without an
Rng draw (§5.3); `kfm_replay stats` counts banana pickups only in the banana supply (it had
counted power-ups too). Replay checksums: unchanged. The content hash changed three times
(zones and the cell size, the hill numbers, the flag numbers and the larger arena table), the
corpus was re-recorded each time and every existing `.checksums` file is byte-identical; two
entries are new, `scripted_4p_hill_3600t` on `hilltop` and `scripted_4p_capture_3600t` on
`outposts` (18 replays). Round length with the scripted generator (`kfm_replay stats
--players 4 --matches 50`, Release): `hilltop`/`hill` 99.1 s mean (p10 75.8, median 100.0,
p90 119.0), no draws; `outposts`/`capture` 173.1 s mean (p10 140.3, median and p90 at the
3 min clock), 18 of 50 drawn at the clock. Four hard reference bots, which ignore objectives,
play `hill` as team deathmatch (`kfm_arena --bots 4 --mode hill --matches 3`: 2 decided by
incidental holds, 1 draw) and score no capture in 3 `capture` matches. Verified locally on
GCC 15: the headless Debug build (33 ctest targets), the dev preset (all 41 incl. the 18-entry
corpus, both soak gates and the real-socket tests), clang-format 23.1.3 and clang-tidy 23.1.4
over the whole tree with zero findings, the no-float check, `kfm_client --no-settings
--autostart --bots 3 --mode hill` and `--mode capture` 180 frames with screenshots (the hill a
white rectangle between its walls, the two bases in team colours with their flags),
`kfm_netsim --clients 6 --mode capture` at 100 ms ± 10 / 2 % for 20 s (701 checksums compared,
0 mismatches, rollback depth max 11) and `kfm_arena --mode hill`/`capture`; MSVC, Apple Clang,
Clang, ASan+UBSan and Emscripten are left to CI. Bench (`kfm_replay bench --ticks 36000
--players 10`, Release, `step()` alone, the shared WSL2 machine): 1.29 µs/tick on
`dojo`/`rounds` (unchanged), 3.46–3.52 µs on `hilltop`/`hill`, 3.35 µs on `outposts`/`capture`,
against 3.32 µs for `team_deathmatch` on `hilltop` and 2.61 µs on `standoff`: the tilemaps'
eight walls and the crowding at the objective cost the time, the modes' own work about
0.2 µs. Follow-ups: an objective-aware reference bot (walk to the hill, run flags); zones in
the observation (step 5); drawing the hill's holder (`HillHolderChanged`); a touch-to-return
rule for the own team, if playtests want one; the grid's `cellSize` check in `compatible`
with the `space` field (step 4).

Progress: M3 landed in v0.17. `engine/net` holds the `ITransport` seam, the loopback
transport with its deterministic emulator and the transport contract suite, the bit-packed
message layer, `NetServer`, `NetClient` (prediction, a 32-state rollback ring plus an anchor,
clock sync and lead control, the event journal, checksum resync, mid-match join, spectating,
reconnect) and `WebRtcTransport` (libdatachannel natively, datachannel-wasm in the browser,
server-hosted `ws://` signaling). `apps/server` is the headless dedicated server, `apps/netsim`
the in-process soak tool, `apps/net-probe` the transport echo check, and `kfm_client --connect`
(natively and as `kfm_client.html?connect=`) the network mode of the M2 client. Exit criterion:
`kfm_netcode_tests` and the `netsim_10c_100ms_2pct` gate run 10 clients at 100 ms RTT, ±10 ms
jitter and 2 % loss with no checksum mismatch or resync and a rollback depth bounded at 11 ticks
(p99 9) against a 32-tick ring, and still no desync at 300 ms and 10 % loss (depth 24); a native
`kfm_client` and the browser page each joined a real `kfm_server` with three bots over WebRTC on
one machine (native: 16 s online across three rounds, 802 rollbacks of depth ≤ 3, 0 mismatches;
browser: online with RTT about 3 ms, 60 fps, 0 console errors, 0 mismatches) and the WebRTC
transport passed its browser echo check (200/200 on each lane, 0 lost). Numbers: the
`kfm_netcode_tests` 60 s soaks give depth max 11 / p99 9 at 100 ms and 25 / 22 at 300 ms, about
53 mispredicted ticks/s per client (the scripted aim jitters every tick), lead 4.7 against 5.3
and 10.7 against 11.3, 0.03 and 0.5 server substitutions/s per client, about 1.1 KB/s up and
4.0 KB/s down per client before transport overhead; a steady 10-player `AuthInputs` is about
85 B and a full snapshot at most 4.7 KB. Verified locally on GCC 15 Debug, Release, ASan+UBSan
(with the vptr check off: the no-RTTI engine has no typeinfo), the server-headless preset,
Clang 21 and Emscripten 6; MSVC and Apple Clang are left to CI (`std::format` and
`std::string::contains` are used for the first time, in `apps/server` and its tests). Sim
change: the `InputFrame` present mask is the roster (§5.4); replay checksums unchanged, bench
unchanged. Follow-ups: `wss://` signaling and tokens from a lobby service (M6), the warm-up
round a joiner lands in (a solo player wins it; the server's own first round is drawn because the
roster reaches the world one tick after start), bots that yield a slot to a human, a shared build
id constant, retracted events undone on screen, reconnect-with-token from the UI, a server-chosen
match config (retired in v0.42: the room's host chooses it, §6.6), multi-match hosting (§6.5), a flush signal on the reliable lane for shutdown, and
a human playtest over a real link.

Progress: M5 landed in v0.30 (the in-repo half; the SDKs and example bots are published from
the bots repository, which is where the exit criterion's tournament is run). `engine/ai` holds
`IAgent`, `Observation` v1 with its fixed-point builder, `AgentHost`, the reference bot, `BotSpec`
and `WasmAgent` over WAMR 2.4.5 with exact instruction metering (§8); `kfm_arena` runs matches
and tournaments to a result line, JSON and `.kfmr` replays; `kfm_server` and `kfm_client` drive
their bot slots through `AgentHost` (the server delaying bots by the humans' lead); the C SDK
seed and the `chaser` example bot under `tools/bots/` are the copy the bots repository starts
from, and a test plays the compiled example against the reference bot. Verified locally: GCC 15
headless Debug and Release (every ctest target incl. the arena smoke tests, the wasm fixture
suite, the soak gates and the real-socket tests), the native dev preset with the client, Clang 21
and GCC ASan+UBSan on the wasm runtime tests, Emscripten for `kfm_arena` as a node CLI; MSVC and
Apple Clang are left to CI (WAMR's `runtime_lib.cmake` builds on both). Numbers: Observation
5748 bytes; WAMR classic interpreter about 820 M instructions/s (Release), a 100 k budget about
0.12 ms; two hard reference bots against two scripted players win 41 rounds to 10. Replay
checksums: unchanged (no sim change). Follow-ups: the browser JS shim so `kfm_client.html` can
run modules; `kfm_arena --team` and the per-instance lifecycle (M5b); bots that yield a slot to
a joining human; a protocol flag so a client can tell bot slots apart in the roster; running
contests at thousands of times real time through parallel processes.

Progress: M4 landed in v0.25. Content is data: `engine/content` parses the TOML under `content/`
(toml++ 3.4.0, exact decimal to Q16.16, names resolved to indices, validation with file:line
diagnostics) into the same `ContentDB`, embedded into every binary and mirrored next to the
executables, hot-reloaded in local play; `MatchConfig{seed, players, arena, mode, powerUps}` rides
in replays (format 3), the join messages (protocol 3) and every host's flags (v0.18, v0.24).
Six power-ups through `PowerUpDef` grants (stat, ability override, projectile, tag) resolved by
`ModifierSystem` with owned `grantedTags`, power-up pickups from arena spawners, the shield rows
(v0.20); Blink, Slam and Parry as a third slot/button through content loadouts or an override,
with their rule rows and the slam reach contact (v0.21); interior walls and pit/slow hazards
through one swept circle-vs-statics path, four new arenas (v0.22); rounds, deathmatch and
survival through a `RulesSystem` mode table with respawn and spawn protection (v0.23); the lobby
settings panel with an arena preview, per-match power-up set and saved settings, the online
read-only view (v0.24); event-driven VFX and procedural audio over SDL3 (v0.19). Exit criterion
("content added without touching system code"): `dojo_powerups`, `pillars`, `corridors`,
`island`, the three modes and the six power-ups are TOML files plus loader keys; adding another
of any of them is a file. Verified: GCC 15 Debug (31 ctest targets incl. the 14-entry corpus,
both soak gates and the real-socket tests), GCC 15 Release, Emscripten 6 (28 tests under node,
the browser page at 60 fps on `pillars`/`deathmatch` with audio open and 0 console errors),
netsim 8 clients on `pillars`/`deathmatch` and 6 on `dojo_powerups`/`rounds_slam` under loss
with 0 mismatches; MSVC, Apple Clang, Clang 21 and ASan+UBSan on CI. Bench (Release, 10
players, step only): 1.23 µs/tick on `dojo`/`rounds` (1.15 before M4), 2.25 µs/tick on
`pillars`/`deathmatch`, 1.22 µs/tick on `dojo_powerups`/`rounds_slam`. Replay checksums changed
in M4 (the content hash in every header; `MatchRules` counters, banana/pickup modifier fields, a
third ability slot and knockback in the state); gameplay of the default match is unchanged, which
each state addition was checked against before the corpus was regenerated. Follow-ups: a
power-up that grants an ability in shipped content, `--powerups` on `kfm_netsim` and the render
probe, a protocol/replay version bump for the M4 snapshot layout (peers already match on content
hash and build id), convex arena shapes, team modes (§12), rooting during Blink/Slam startup,
music and distance attenuation, listening to the mix on a machine with audio, a human playtest.

Progress: M2 landed in v0.15 (pending human playtest of the exit criterion). The client opens
an SDL3 window with a GL 4.3 core context (4.1 on macOS) through the vendored glad loader,
draws the arena, players, bananas, pickups, blob shadows and effects with sokol_gfx from a
`RenderState` built from two sim states, interpolation alpha and the tick's events, and runs
the same code in the browser as `kfm_client.html` (WebGL2, `desynchronized` context,
`requestAnimationFrame` loop). Players are GPU-skinned from a generated rigged test model with
six clips and a crossfading state machine, or capsules on request. Up to ten local slots: two
keyboard profiles and gamepads with hotplug, press-to-join lobby, scripted bots filling the
rest, restart with the same or a new seed. Dear ImGui HUD (scores, countdown, banners, ammo
pips) and F1 overlay (frame and tick statistics, entity inspector, pause and single step, time
scale, interpolation policy, live content tuning printed back as C++). Rendering is checked
without a human by `kfm_client --exit-after-frames --screenshot`, `kfm_render_probe --exit-on
<event>` and the Linux xvfb smoke tests; the browser by `tools/web/check_browser.py` in
headless Chromium (WebGL2 context attributes, no console errors, steady 60 fps frame pacing
under SwiftShader, keyboard input moves and throws). `kfm_replay stats` gives the first
feel-tuning numbers (§12): no content change in M2, replay checksums unchanged, sim bench
unchanged (about 930k ticks/s `step()` alone in Release on the development machine). Verified
locally on GCC 15 Debug, Release and ASan+UBSan, Clang 21 and Emscripten 6; MSVC and Apple
Clang are left to CI. Follow-ups: the upper/lower body split and a slumping body after
elimination (§7), the §12 catch-at-full-ammo decision, mouse aim through `screenToGround`,
Linux audio (SDL `alsa` feature), instancing when draw calls grow, and the human playtest.

Progress: M1 landed in v0.13. `engine/sim` runs the full §5.3 pipeline as ten system files:
FFA round flow (countdown, round end, respawn, scoring, match end), input decoding with press
edges, Dash and Throw through the `AbilityBehavior` table, movement with the arena clamp,
banana flight, return steering and wall bounces, a uniform-grid broadphase checked against
brute force with an exact swept-circle narrowphase, the rules table (perfect catch judged
against the press sub-tick, damage with `HitStun`, auto-catch, pickups), modifiers and
lifecycle. Content is the `constexpr` `ContentDB` with its hash. `kfm/sim/scripted_inputs.hpp`
drives `kfm_replay record-scripted`, `kfm_replay bench` and the scenario tests; the corpus holds
`scripted_10p_3600t` and `scripted_4p_1200t` beside the empty replays and passes with identical
checksums on GCC 15 Debug, Release and ASan+UBSan, Clang 21 Debug and Emscripten on Linux
(MSVC and Apple Clang are left to CI). Bench, 10 players, scripted match, one core of the
development machine (Linux under WSL2): about 900k ticks/s (1.1 µs/tick) for `step()` alone in
Release, about 480k ticks/s including input generation; Debug about 200k ticks/s. That is
roughly 15 000× real time, so rollback depth and server match density are not sim-bound.

Progress: M0 landed in v0.6 (build skeleton, `core`, a minimal `sim` with the visitor serializer,
replay codec, `kfm_replay` and the determinism gate over `tools/replay-corpus`, CI workflow).
Verified with identical per-tick checksums on Linux GCC/Clang, Windows MSVC and macOS Apple Clang
in Debug and Release, ASan+UBSan and Emscripten. C++23 probe passed on all five toolchains and the
baseline moved to C++23 (§9).

---

## 12. Risks and open questions

- **Fixed-point ergonomics.** Mitigate with a complete `Fixed` operator set, LUT trig and unit
  tests against a `double` reference.
- **Very high RTT (v0.44).** Measured to 500 ms (§6.2): the lead control holds and a jittery link
  keeps its inputs on time since the jitter-aware band, but past ~480 ms the rollback depth
  exceeds the 32-tick ring and rollbacks re-simulate from the verified anchor every time, which
  costs CPU in proportion to the depth (fine natively and for one local player; a browser on a
  slow phone at 500 ms is untested). Mitigations if it shows: a larger ring for high-RTT links,
  or the input delay (§6.2) raised automatically above some RTT. The netsim jitter is uniform;
  real links have spikes, which the floor follows half way per report and forgets in seconds.
- **Visual noise from predicting 9 remote players.** Mitigate with smoothing, the optional
  1–2 tick input delay, and, if needed, rendering remote players 1–2 ticks behind confirmed.
- **Catch feel under latency.** Catcher-favouring is inherent; the thrower sees the catch
  ≈ RTT late. Add anticipation VFX on the dash so the result reads as intentional (v0.19: the
  DashAnticipation ring and flash at the press, §7).
- **Snapshot growth** if entity caps rise; fine up to ~100 KB with LZ4. Measured (v0.17): at
  most 4.7 KB uncompressed with every pool full, so LZ4 is deferred until a cap rises (§6.3).
- **Lobby/matchmaking/NAT** is a service problem behind the connection-token boundary; the
  service is designed in §13.5 (v0.37). No matchmaking: rooms are created and joined by code,
  the creator picks the region.
- **Browser as the primary client.** One to two frames more input latency than a tuned native
  client, Safari quirks (audio gesture, gamepad, WebGL), tab throttling (covered by snapshot
  resync), no threads without COOP/COEP headers (the sim is single-threaded anyway), WebGL2 as
  the floor. Fixed-point determinism holds in WebAssembly for free, which floats would not.
- **Always-on servers.** The web needs dedicated servers and a signaling service running 24/7.
  The sim is cheap enough that one small VPS per region hosts hundreds of matches, but it is an
  ongoing cost and an ops duty that a Steam peer-to-peer game would not have. At friends scale
  (v0.37) it is one small VM; egress (~100 KB/s per match) is the one line that grows with play.
- **Monetisation on the web is weak** (portals, ads, cosmetics). If revenue matters later, the
  Steam build is the lever, and the seams keep it cheap.
- **3D renderer cost.** A skinned-mesh renderer is weeks of work where a sprite batcher is
  days; it is the main schedule risk in M2. Mitigate by running M2 on capsules first, keeping
  the one light and blob shadows (no shadow maps), and keeping the sprite fallback (§7).
- **Stack in WebAssembly.** One `step()` keeps about 40 KB of per-tick scratch on the stack
  (contact list, broadphase grid, contact sort keys), which is how the "no heap after init" rule
  is met. Emscripten's default stack is 64 KB and overflows silently in Release, so every wasm
  executable links with `-sSTACK_SIZE=1MB` (v0.13); the browser client must too.
- **WebGL2 limits.** Skinning uniform budgets and texture formats vary by browser; keep joint
  counts low and test the wasm build on every M2 change.
- Decided: 60 Hz tick with sub-tick input timestamps (§5.11); WebRTC as the single production
  transport (§6.4); web first, native as the dev build, Steam optional (§13).
- Decided (v0.9): HP with banana damage defaulting to a one-hit kill (§5.6); `constexpr`
  content behind `ContentDB` until the M4 loader (§5.10); spatial-hash broadphase with a
  brute-force oracle (§5.3).
- Decided (v0.10): HP semantics, `HitStun`, banana grounds on hit, `PlayerHit` event (§5.6).
- Decided (v0.11): real-time low-poly 3D presentation over the 2D sim, replacing pre-rendered
  2.5D sprites (§7).
- Decided (v0.13, M1 integration): nothing moves or resolves outside `Playing` (§5.7); the
  perfect catch ignores an own outbound banana (§5.6); `HitStun` counts the hit tick (§5.6);
  idle stick iff `moveMag == 0` (§5.4); `PerfectDash` covers the sub-tick end of the window
  (§5.5); the wall bounce stays in `ProjectileSystem` until arenas have interior walls (§5.6).
- Decided (v0.15, M2): world basis and fixed arena camera, interpolation/snap/smoothing
  constants, height curves, facing and effects (§6.8, §6.9, §7); GL-only sokol backends with
  hand-written GLSL, vendored glad loader, GL 4.3 core natively (§7, §10); input sampler
  semantics and default bindings (§5.4); dependency sourcing (§10).
- Decided (v0.16): dash attack. Dashing into an enemy while holding a banana damages it,
  through rule row 25 on Player × Player contacts (attacker as A, both orientations, press-time
  tie-break, no pickup, ammo kept by default), switched by `DashAttackDef` (§5.3, §5.6, §5.10).
- Decided (v0.16): catch only below `ammoMax`; a full-ammo perfect dash into a non-own banana
  takes the hit (§5.6). The record of the alternatives, as it stood open in v0.15: a perfect
  catch at full ammo destroyed the banana, so with `ammoMax = 1` the thrower stayed empty-handed
  until it found a pickup. Alternatives: catch only below `ammoMax`, or drop the caught banana
  as a pickup. Numbers from `kfm_replay stats` over 20 scripted matches each (v0.15): 15% (2p),
  30% (4p) and 40% (10p) of perfect catches happened at full ammo; at 4p about half of those
  throwers were eliminated before regaining ammo and the rest waited a median 1.1 s (p90 6 s),
  about the same as after any catch, because the thrower loses its banana either way. The
  round's banana supply (held + in flight + pickups) never reached zero with two or more
  players alive, pinned by `banana_supply_scenario.cpp`; the cost of the old rule was scarcity
  (lowest supply per player in a round: median 1.0 at 2p, 0.5 at 4p, 0.3 at 10p). The v0.15
  recommendation was to drop the caught banana as a pickup at the contact point; the owner chose
  "catch only below `ammoMax`", which sends the 30–40% of former full-ammo catches through to
  `DamagePlayer` (the hit grounds that banana into a pickup, so the supply floor still holds).
  First feel-tuning pass (v0.15): no content change. Catch attempts convert at 82–86% and
  almost every miss is a press stamped a fraction of a tick after impact, not a short window;
  short rounds (4p median 4.6 s) come from the scripted bot's 2 Hz near-perfect throwing, not
  from the content (a 4× slower thrower gives a 9.9 s median). Human playtests decide the rest.
- Decided (v0.27): bananas pierce by default. `[boomerang] pierce = true` in the base kit and
  `defaultContent()`, so a banana hit damages and the banana flies on, out and back to its
  thrower as on a miss; a piercing banana that crosses several monkeys hits each once (`HitStun`
  still stops a double hit on one victim). Shield blocks, slams, a gone owner and
  `maxReturnTicks` still ground bananas, so pickups remain but are rarer. The supply floor still
  holds: a banana that hits stays in flight and is counted as supply until it comes home or
  grounds. The `piercing_banana` power-up had nothing left to grant and is removed; the
  `ProjectileModifier::pierce` grant stays for content that turns the base switch off (both removed in v0.28). Removing
  it shifts the indices of `triple_banana`, `shield` and `fast_hands` (and their
  `MatchConfig::powerUps` bits); that is safe because the content hash pins indices in replays
  and on the wire, the corpus was regenerated and saved lobby settings name power-ups rather
  than index them.
- Decided (v0.28): no pierce switch. With every banana piercing, `[boomerang] pierce` and the
  projectile grant's `pierce` only offered a way back to the old rule, and the owner chose to
  drop them: the fields leave `BoomerangDef`, `Boomerang` and `ProjectileModifier`, the loader
  rejects the keys as unknown, and `DamagePlayer` never grounds a banana. Shield blocks, slams,
  a gone owner and `maxReturnTicks` are now the only ways a banana grounds.
- Decided (v0.26): bots are a public contract, a sandboxed and instruction-metered wasm module
  behind a four-export C ABI; the SDKs, example bots and contest live in a separate repository;
  no training stack in the engine (ONNX Runtime and the nanobind env dropped); human-likeness is
  a host setting (input delay equal to the human lead in mixed matches, none bot versus bot)
  (§8, §10, §11).
- Decided (v0.30, M5): WAMR 2.4.5's classic interpreter with exact fuel read-back (a
  configure-time patch), 100 k instructions per tick, 10 M for instantiation and `kfm_init`,
  4 MiB modules, 16 MiB memory; `Observation` v1 is pinned in `docs/bot-abi.md`; bots are named
  by `BotSpec` and seeded per slot by one formula in every host; the server delays bots by the
  humans' lead (§8, §10, §11).
- Decided (v0.36, M6b then, M7 since v0.40): the ladder is a hosted service in the bots repository over tagged
  releases of this one; per-user tokens, teams as entrants, source-only C++ submissions built in
  a throwaway container, five submissions kept plus the active one, a `contest` mode as the
  season's knob row, ratings per entrant over rotated two-sided matches reproducible from seed
  plus modules, a Rust CLI on PyPI (§8, §11). v0.59: a season is a time window, not a release;
  every match records its release tag, replays are served with their release's content and
  viewed in that release's client (the page under a versioned path per tag), the `contest` row
  may change inside a season, and ratings persist across a release.
  v0.63: the service's shape (§11 "M7 step 3 design"): Rust, Postgres, S3-compatible blobs, a
  jobs table as the queue, GitHub or Google OAuth, gVisor on a dedicated build VM, Swiss-style
  rounds under Glicko-2. v0.65: Glicko-2 decided (per-game periods, the release widening
  constant 100), seasons as time windows, validation in the ranked 5v5 shape.
  v0.66: replays are kept 24 hours; the host is `bots.kfm.minip8.com`; the CLI is `kfm-cli` on
  PyPI.
  Open: how long a release's content and page are kept, and whether a fog overlay belongs in
  the viewer.
- Open (M7 step 6 follow-ups, v0.68, the replay viewer of §7): the first seek across an
  unvisited span still steps all of it (keyframes are recorded as playback reaches them; ~45 ms
  for a whole 10-minute replay in Release, more in Debug or on a slow phone), which a background
  pass could pre-record if it shows; effects and animation run in wall time, so at 0.25x and 4x
  they are slow or fast against the sim (the sim speed is right); player names in replays (the
  portal knows the entrants: a `?names=` parameter is the likely shape, since the `.kfmr` format
  carries none); a main-menu entry or a file picker; seeking with a pad's stick; the fog overlay
  (above); the bar's two-row layout (below about 987 px of display) is unit tested, not seen on
  screen, because the native window is 1280×720.
- Decided (v0.37, M6a): a Rust lobby service in this repository is the only public TCP
  endpoint and owns rooms, tokens, signaling and the server registry; servers dial it; one
  `kfm_server` process per match under systemd; Google Cloud Compute Engine first and
  Hetzner Cloud after, with the deployment portable by construction; Cloudflare Pages, Caddy,
  Sentry's free tier (§13.5). Open: whether the ladder's (M7) accounts and the lobby ever share an identity
  (a guest id is enough to play), and whether the lobby moves to its own host once there is a
  second region.
- Open (M5 follow-ups): the browser JS shim for bot modules in the client (metering best effort
  there); bots that yield a slot to a joining human; contests at thousands of times real time
  through parallel processes (§8). Two sim-side sources of slot bias found by the bot fairness
  pass (§8) are left as they are, because changing either regenerates the corpus: the spawn
  assignment `order[(rank + offset) % 10]` puts four players on uneven arcs of the dojo's ring
  (slots 1 and 2 face each other, 0 and 3 sit 36° apart on odd offsets), and equal-time,
  equal-press Player × Player contacts resolve by pool index. A per-round shuffle of present
  slots onto spawn ranks from the world Rng, or spawn groups (M5b), would fix the first;
  tournaments rotate slots meanwhile. Measured after spawn groups landed (v0.32, 4 hard bots,
  fixed slots, 200 matches): `dojo` keeps one group and its bias (wins 28/77/64/31 by slot);
  on `standoff` the two teams of `team_rounds` win 94 and 100, so groups remove the side bias
  between teams, but its free-for-all goes 68/84/19/17: groups alternate free-for-all slots
  between the sides, a same-side bias in place of the arc bias. Spawn groups therefore fix
  team play only; the per-round shuffle stays the candidate for free-for-all and remains open
  (it regenerates the corpus).
- Open (M5b step 2 follow-ups, v0.32): a non-spawnable power-up flag (or a set the spawners
  never draw from) so a friendly-fire baseline can ship without changing `dojo_powerups`'
  draws; the reference bot holding fire when a teammate is on the line under friendly fire;
  per-team places and result lines in `kfm_arena` (step 5's `--team`).
- Open (M5b step 3 follow-ups, v0.33): the reference bot ignores zones and flags, so a bot
  `capture` match scores nothing and `hill` is won by incidental holds; an objective-aware
  reference bot needs the zones, which Observation v1 does not carry (step 5 adds them);
  whether a dropped flag should go home when its own team touches it (classic capture the flag
  does; v0.33 returns it only by the clock); content-level fairness of a zone (`compatible`
  only counts zones, so `outposts` also accepts `hill` with the hill on a base).
- Open (M5b step 4, v0.34, the grid numbers as observed): the reference bot never loses a
  monkey it can see coming on the grid (a cell-wide side-step beats every banana content can
  fly), so all 8 bot matches above hit the cap; the throw range is 4.8 cells, not 6, because
  `outbound_ticks` is the `BoomerangDef`'s; the 1-tick variant's bananas fly at about 1.07 cells
  per turn (the projectile multiplier's limit of 4), not 2, so it plays a different game from
  the 15-tick mode it should mirror. Candidates, to settle with step 5's grid-aware bots: a
  per-mode `BoomerangDef` override or flight counted in turns; a faster banana against the walk
  (at 2 cells per turn a seen banana is always one side-step away); the in-cone catch.
  Mud has no effect on the grid (a walk is a cell per turn); whether it should cost a turn is
  open too.
  v0.79 takes the first two up for the contest (§11 "Contest design": a speed limit of 8 for
  the projectile multiplier and a per-mode outbound and turn rate), designed and not built.
- Open (M5b step 5, v0.35, the fog and grid numbers as observed): with grid-aware reference
  bots under `own` fog every grid match ends (above, §8), but the fights are close-range: the
  bot only throws from three cells or nearer, because from farther a seen or heard banana is
  side-stepped; it is the walls and the fog, not the banana, that decide. In 20 rotated 5v5
  matches on `grid_rounds_1t` both sides won 10 (no side bias), a whole match (first team to
  three rounds) lasting 1140 ticks on average, countdowns included. The 1-tick variant still flies its bananas at about 1.07 cells per turn, so
  the same `vision` block gives it three turns of hearing where the 15-tick mode gets one and a
  half: the per-mode flight (or flight in turns) of step 4 stays the first lever, then hearing 2
  for the 1-tick mode. Calls are cheap (894 in three 5v5 matches from bots that call at most
  once per eight turns), and a call on a latch tick turns the caller's cone for that turn; an
  alphabet mask and a call cooldown measured in turns are the contest knobs to try. Whether the
  catcher's catch needs the in-cone restriction is still untested: the reference bot catches
  only incidentally. The open question of a shared team array (§8) waits for a contest bot that
  needs one.
- Decided (v0.29): a mode is content along five axes, each with its own seam (§5.3); teams ride
  in `MatchConfig` and reach the rules table as a relation mask, friendly fire being a tag the
  mode's baseline grants (§5.6); loadouts are their own match axis (§5.10); the grid is a
  `Space` of the same sim with continuous bananas (§5.12); the contest shape is a squad of
  one-instance monkeys under fog with in-game calls (§8). Checksums: the relation mask, the
  radius stat and the loadout axis keep every existing checksum; the commitment fields and
  `modeData` change them once when they land (corpus regenerated); `kMaxModes` changes the
  content hash only.
- Open (M5b): the grid numbers (§5.12: cone, hearing, radii, `turnTicks`, the catcher's
  cooldown; shipped in v0.35 at §8's starting values, first observations above), tuned with
  `kfm_arena` and recorded here when they settle; whether the catch also
  needs the in-cone restriction; whether `hot_potato` and `infection` earn a kind; a shared team
  array (§8) if squads prove unwritable without one. Fog for humans is not enforceable and stays
  out.
- Open: WebTransport as a later replacement for WebRTC; mobile browsers and touch controls.

---

## 13. Platform strategy: web first, native as the dev build, Steam optional

### 13.1 Why the browser is the primary shipping client

A party brawler lives or dies on how fast a group gets into a match. A link that puts a friend in
the arena in ten seconds, with no download, no installer, no account and no OS warning, beats every
other distribution property for this genre. The browser also gives cross-platform for free
(Windows, macOS, Linux, ChromeOS), instant updates, link-based virality, and web game portals as a
distribution channel. The architecture pays nothing for it: the sim, netcode and renderer were
already designed to run there.

What the browser costs, honestly:

- One to two frames more input latency than a tuned native client (compositor, forced vsync via
  `requestAnimationFrame`, ~1 ms timers). §6.8 lists the mitigations. The netcode hides network
  latency, not this; it is small next to the join-friction win.
- Browser quirks: audio needs a user gesture, Safari lags on everything, a throttled background
  tab pauses the sim (snapshot resync covers it), WebGL2 is the floor, download size matters.
- Always-on dedicated servers and a signaling service: an ongoing cost and ops duty.
- Weak monetisation (portals, ads, cosmetics) and no storefront discovery.

### 13.2 Native without Steam: build it, do not ship it

A native client stays in the repo as the development, debugging and profiling build: Tracy,
sanitizers, `netsim` with ten clients, `kfm_replay` and the match runner all live there (the
replay viewer is the client's and runs in the browser too, v0.68), and it is the reference
the browser build is checked against. Shipping it without Steam adds installers for three
OSes, Windows SmartScreen and macOS notarisation (paid certificates), auto-update and crash
reporting, and buys one to two frames of latency and no audience. For
players it is a worse experience than Steam (no overlay, friends, invites, cloud) and a
higher-friction one than the web. itch.io is the one reasonable non-Steam native channel, as a
side door, not a launch.

### 13.3 Steam, if it ever happens

Steamworks is a native C++ SDK, so the integration is cheap and confined to the two seams:

| Steam feature | Where it lands | Effort |
|---|---|---|
| Networking (`ISteamNetworkingSockets`) | `GnsTransport` built against the Steamworks SDK (API-identical to open-source GNS); adds Steam Datagram Relay | days |
| Lobbies, invites, rich presence, friends | Steam implementation of `IPlatformServices` | 1–2 weeks |
| Remote Play Together | free; the local mode becomes playable online by streaming | a checkbox |
| Steam Input / gamepads | SDL3 already handles it | none |
| Achievements, stats, cloud saves | `IPlatformServices`; small | days |
| Overlay | hooks the GL/D3D/Vulkan presenter; test with sokol | none |
| Steam Deck | the Linux build; verify controller-only UI and text size | days |
| Build and deploy | SteamPipe depots per OS from CI via `steamcmd` | days |
| Admin | partner account, US$100 app fee, tax interview, store page, build review | weeks of calendar |

Only one of {open-source GNS, Steamworks SDK} links into a binary; a CMake option picks it. A
Steam build and the web build share a match through the dedicated server, which runs both
transports.

### 13.4 What is shared between the web build and any native or Steam build

Everything below the platform seam: `core`, `sim`, `ai`, `presentation`, the message layer,
prediction and rollback, content, tests, the dedicated server. sokol_gfx with sokol-shdc compiles
one shader source to GL, D3D11, Metal and GLSL ES 3.0; SDL3 runs on Emscripten; fixed-point makes
the WebAssembly build bit-identical to native. By volume, roughly 90% of the code and all of the
game.

| Concern | Web | Steam |
|---|---|---|
| Transport | WebRTC data channels (same C++ code natively and in wasm) | same WebRTC, or GNS/Steamworks networking for Steam relays |
| Identity | guest ids, later own accounts | SteamID |
| Lobby, invites, presence | own lobby and signaling service, room codes in URLs | Steam lobbies, overlay invites, rich presence |
| Hosting | dedicated servers | dedicated servers, or an SDR listen server for private lobbies |
| Distribution | static host + CDN, HTTPS | SteamPipe |
| Persistence | IndexedDB / localStorage | Steam Cloud |

Each platform implements `ITransport` (§6.4) and `IPlatformServices` (identity, session
directory, invites, presence, achievements, cloud). Nothing else knows which platform it is on; a
`null` implementation serves tests and the headless server. v0.43: `IPlatformServices` also copies
text to the clipboard (`copyText`) and offers a link to the share sheet (`canShare`, `share`):
SDL's clipboard natively, `navigator.clipboard` and the Web Share API (touch devices only) in
the browser, through hooks the app injects so `kfm_platform_services` stays SDL-free. Since the
architecture audit the seam also holds everything else in the client that differs between the
two: named preference texts (`loadPref`/`savePref`: the settings and the guest id), the page's
address (`pageUrl`, for invite links), the replay viewer's download (`startDownload`,
`pollDownload`, `cancelDownload`: a local file natively, a CORS fetch in the browser), and two
traits, `canQuit` (no Quit in a browser tab) and `localFiles` (hot reload natively only).
`LocalPlatformServices` takes them as one `PlatformHooks` value, which the client builds in
`apps/client/client_platform_native.cpp` (SDL) or `client_platform_web.cpp` (`EM_JS`), CMake
compiling one of the two; `main.cpp` holds no `EM_JS` and no `__EMSCRIPTEN__` branch.

### 13.5 Hosting: dedicated servers, decided

Browsers can host a listen server over WebRTC, but the host tab gets throttled, the host has zero
lead, and host migration is real work. Dedicated servers are the cleaner answer and the
architecture already assumes them: clients connect out, so no TURN relay is needed and NAT is a
non-issue; the server is the authority and the bot host; a match costs ~100 KB/s and
a sliver of a core, so one small VPS per region (EU, NA, Asia to start) hosts hundreds of matches.
The lobby/signaling service is a small WebSocket endpoint that issues room codes and connection
tokens and tells the browser which server to dial.

**The M6a deployment (v0.37).** Three parts, each portable, sized for friends and shaped so
that growing is adding machines rather than changing code.

- **The client page is static.** `kfm_client.html` with its wasm and JS is published by the
  `wasm` CI job to Cloudflare Pages on a tag (free, a global CDN, TLS and a custom domain; a
  `_headers` file sets `application/wasm` and immutable caching for hashed files; no COOP/COEP,
  the client is single-threaded). A room link is `https://<host>/?room=CODE`.
- **The lobby service** is one Rust binary (Tokio, axum, tokio-tungstenite) in
  `services/lobby/` of this repository, because its protocol tracks the server's, and the only
  public TCP endpoint: a single `wss://` socket behind Caddy, which handles Let's Encrypt. It
  owns room codes, connection tokens, signaling and the server registry, all in memory (a room
  is a few hundred bytes; a database arrives with accounts, not before).
  - Servers dial it: each `kfm_server` opens an outbound `wss://` connection with a shared
    secret and sends `register{region, build_id, content_hash, max_players}`. A game VM
    therefore needs no inbound TCP, only its ICE UDP range. A server that drops off the socket
    leaves the registry and its rooms close.
  - A client dials it to create a room, `create{region}` → `{code, token}` (the lobby picks a
    free registered server in that region, reserves the room on it and hands the server the
    token), or to join one, `join{code}` → `{token}` or a refusal (unknown code, full, build or
    content mismatch), then exchanges signaling over the same socket: the lobby relays the
    client's `offer` and `candidate` messages to the server and the server's `answer` and
    `candidate` back, keyed by room and peer. The JSON message set of
    `kfm/net/webrtc_signaling.hpp` is kept and wrapped in a routing envelope.
  - A room code is five letters from an unambiguous alphabet, unique while the room lives; a
    room expires ten minutes after its last peer leaves. A token is 128 random bits bound to
    one room, issued once per join and valid until the room closes; because the lobby tells
    the server its tokens at reservation time, `ServerConfig::acceptToken` is a lookup, not a
    round-trip (§6.7). The region is the room creator's choice from the registered regions, EU
    by default; there is no latency-based matchmaking.
- **The game servers** are one small VM per region (Compute Engine `e2-small`, Debian, a
  reserved static external IP, a firewall rule for the ICE UDP range) running `kfm_server`
  under systemd, one match per process as built (§6.5): a unit template keeps a pool of idle
  processes per VM, each registered with its own capacity. `MatchHost` replaces the pool only
  if a VM runs out of processes before it runs out of CPU. The EU VM also runs Caddy and the
  lobby. Start with one VM; a region is one more VM running only servers.

Code this needs: `WebRtcTransport`'s server side takes an injectable signaling source (the
built-in `rtc::WebSocketServer` behind `signalingPort` stays for development, tests and
`kfm_net_probe`; a `--lobby wss://… --lobby-secret S` mode on `kfm_server` feeds it the
messages relayed over the lobby connection and registers the process); the client takes a
lobby URL and a room (`?room=CODE` in the browser, `--room CODE [--lobby URL]` natively; the
direct `--connect ws://` path stays for development); the `web` `IPlatformServices` (identity:
a guest id kept in `localStorage`; session directory: the lobby) is the client's seam for it,
`null` serves tests, and the server talks to the lobby directly from `ServerHost`.

Providers: Google Cloud Compute Engine first (credits), Hetzner Cloud when they end. The rule
that keeps the switch a day's work: everything is a container image (`deploy/Dockerfile.server`
from the `server-headless` preset, `deploy/Dockerfile.lobby`) plus systemd units and a
Caddyfile under `deploy/`, with VM creation scripted (`deploy/gcp/`); no provider-specific
service (no managed database, cache, load balancer or serverless runtime; Cloud Run has no
UDP, so it could only ever host the lobby). No spot or preemptible VMs: they are killed
mid-match. Egress is the one cost that grows with play (~100 KB/s per match).

Telemetry and crash reporting: Sentry's free tier. Its browser SDK in `shell.html` catches
wasm traps, unhandled exceptions and failed loads; its Rust SDK covers the lobby. `kfm_server`
keeps its per-event log lines and periodic stats line, collected by journald on the VM, and
sim events feed nothing external. Monitoring is a lobby `/healthz` reporting registered
servers. Revisited at scale.

Scale path: (1) one VM; (2) a VM per region; (3) `MatchHost` and the lobby on its own host
(registration is an outbound socket either way, so the protocol does not change); (4) a
database when accounts arrive. The ladder's (M7) identity provider is its own; whether the lobby ever
shares it is open (§12).

**As built (v0.38).** The wire format is `docs/lobby-protocol.md` (normative; the service, the
transport and the apps follow it, and its resolved ambiguities are recorded there: a missing
required key on a first message is refused `bad_request` while the same after `registered` or
`joined` is a protocol error; `create` picks among free servers whose build and content match;
`capacity` is the total of human slots a process offers and the lobby counts occupancy by open
client sockets). `services/lobby/` is the Rust binary `kfm-lobby` (Tokio, axum 0.8, serde_json,
clap, tracing; Sentry behind a default cargo feature, started only with `SENTRY_DSN`): all state
behind one mutex never held across an await, one task per socket with the token-bucket rate
limit and the frame size limit, `--listen`, `--secret`/`KFM_LOBBY_SECRET`,
`--room-ttl-seconds`, `--log-level`, `/healthz`. Servers dial `<lobby>/server` and clients
`<lobby>/client`; both apps take the base URL. The server side is §6.4 "Lobby modes" and §6.5;
the wiring lives in `LobbyAdapter` next to `ServerHost`, not inside it (correcting v0.37).
The client (apps/client, v0.38): `--lobby URL` (default the CMake cache variable
`KFM_LOBBY_URL`, empty unless the build sets it; the browser page takes it from `config.js`
next to it, `window.KFM_CONFIG = { lobbyUrl, sentryDsn }`, which the wasm build also writes
with the configured value so a local `serve.py` has one), `--room CODE` (any case) and
`--create [--region R]` (default `eu`); the two exclude each other and `--connect`, which stays
the direct development path; `--token`, `--stun`, `--spectate` and `--players` keep working,
and in lobby mode the lobby's token replaces `--token`. In the browser `?room=CODE` and
`?create&region=us` go through the existing query-to-flags conversion, so an invite is the
page URL with `?room=CODE` (plus `&lobby=` when the page had one); natively it is `--lobby URL
--room CODE`. `NetSessionConfig::lobby` (a `LobbyTarget`) and an `ILobbyJoin` view of the
transport (room, token, error) attached with `NetSession::attachLobby()` carry the join: the
token is applied before the first `pump()` that sees the room, and a refusal never sends a
Hello; the loopback session test fakes a lobby-mode transport that withholds Connected until
`joined`. (v0.43 moved this Online section to the main menu's Play online screen, §7.) The lobby settings panel gains an Online section when a lobby URL is configured
(Create room with the region cycled over the regions the lobby lists, asked through a
short-lived transport in `LobbyRequest::Kind::Regions` on every visit to the local lobby,
`eu`/`us`/`asia` until it answers, and activated by Enter, a pad's Start or a button; a Join field as an ImGui upper-case text input over the room alphabet); the region is
saved with the settings. In a match the HUD shows "Room ABCDE (eu)" and the invite; before it,
the status window shows the join in progress, then the code with a Copy button, and every
refusal or close in plain words ("Room ABCDE not found", "Room ABCDE is full", "No server
available in eu", "Version mismatch: reload the page or update the client", ...); Escape
returns to the local lobby, whose Start rejoins a room made with `--create` (v0.43: Escape returns
to the main menu and the Play online screen's code field keeps the room for Join). `shell.html`
loads `config.js`, adds `--lobby` from it when the query has none (v0.68: `?replay=<url>` maps
onto `--replay` by the same generic query-to-flag rule, so the ladder's links needed no page
change; the page fetches the replay with CORS, and the bucket allows GET from the page's
origin) and, only with a `sentryDsn`, the pinned Sentry browser bundle (subresource integrity, `crossorigin`) wired to
`window.onerror`, unhandled rejections and Emscripten's `onAbort`. `IPlatformServices`
(`kfm/platform/platform_services.hpp`, the SDL-free target `kfm_platform_services`) is the
minimal seam: a guest id generated once and kept in `guest_id.txt` in SDL's pref dir or
`kfm.guest_id` in localStorage (per run under `--no-settings`), and the lobby URL (§13.4 lists
what it holds since); `Null`
serves tests. The guest id is sent nowhere yet; M6b's nicknames ride beside it and M7 binds it to an account.
Deployment as built: the VMs run Ubuntu 24.04 LTS and only containers (correcting v0.37's
Debian), so the host compiler is irrelevant; the units run the GHCR images with host
networking so ICE sees the real interface; `kfm.env` carries the secret, region, name prefix,
bot count, image tag and, on a server-only VM, the EU lobby's `wss://` URL; the ICE UDP range
is 40000-40099 by default and the units pass it with `--ice-ports`; `--stun` defaults to
Google's public STUN. The release workflow publishes the page as `index.html` with its `.js`
and `.wasm` under `assets/<sha256>/` (immutable caching; the names alone never change between
builds) to the Cloudflare Pages project with `config.js` and `_headers` beside it (v0.39; the
page's custom domain is attached in the dashboard); the lobby's hostname is a DNS-only A
record on the same domain, or a free dynamic-DNS name that Let's Encrypt certifies.
`deploy/README.md` is the day-one runbook; `deploy/hetzner.md` the provider switch.
Lobby protocol v1.1 (v0.41, M6b step 1): the `regions` reply carries `list` (every registered
region, the distinct regions of every registered server, free or busy) and `free` (the subset
with a server that has no room and a `capacity` of at least 1), both sorted and
comma-separated; the service derives both from its server registry under the one mutex.
`/healthz`'s `regions` is unchanged (the free list): a second key there would serve no
consumer yet, and keeping it is the backward-compatible choice. A v1 client reads only `list`
and may try a busy region (refused `no_server`, as a creator racing another always could be);
a v1.1 client against a v1 lobby has no `free` and assumes every listed region free, which is
right because a v1 `list` is the free list. The transport returns
`std::vector<LobbyRegion>` (`name`, `free`) in the lobby's order (an item of `free` that `list`
lacks is ignored); the client's `LobbyMenu` keeps a `RegionChoice` per region (kRegions, all
shown free, until the lobby answers), the Create room row reads "eu" or "us (no server)" and
still creates in any region, and a `no_server` refusal is shown as before ("No server
available in us"). An empty reply keeps the previous list.
Lobby protocol v1.2 (v0.44, M6b step 4): reconnect from the UI goes through the lobby, since
the lobby counts a closed client socket as leaving and the server only accepts the tokens the
lobby announced. The lobby remembers every token it issued for a live room (seated or not);
`join` gains an optional `token`, and a join with one the room issued is a rejoin: the lobby
answers `joined` with the same token and announces the client to the server as a new `peer`
with that token, so the server's `LobbyLink` accepts it again and `NetServer`'s reconnect
reclaims the reserved slots (§6.6). A rejoin is exempt from `capacity` (the server still holds
the player's slots, and its `Hello` judges otherwise) but not from the 64-socket cap; a socket
still seated under the token (its owner noticed the drop first) is closed with
`closed`/`replaced` and reported `peer_gone` before the new `peer`; an unknown token is
refused `bad_token`. Re-signaling to the server directly was the alternative and is not
taken: a server in lobby mode has no public signaling socket, and a second path would bypass
the lobby's token set. A v1.1 lobby ignores the key and seats the client as a new player.
Lobby protocol v1.3, part one (v0.70, the audit's abuse limits): `create` is unauthenticated
and reserves a whole server process, and a room code is the only admission control, so the
lobby now limits each client address (an open-socket cap, a cap on live rooms it created, a
token bucket on `create`/`join` and a separate one on joins refused `unknown_code` or
`bad_token`, which bounds code guessing), refused with the new reason `limited`; the address
is the TCP peer unless that peer is a configured `--trusted-proxy` (Caddy on 127.0.0.1, whose
`X-Forwarded-For` it then reads, last entry only). A room keeps at most 128 tokens, evicting
the oldest unseated one. A v1.3 server names its version in `register`, learns the lobby's from
`registered`, and reports `players` (its room's peers with both data channels open) on every
change; the lobby holds a room while that is above zero, counts it toward `full`, and holds a
room no game connection ever reached for one minute instead of ten, from its creation as well
as from its last socket, so a create that never plays frees its server quickly while a running
match keeps the full TTL for rejoins. Rooms of an older server keep the v1.2 hold. The
reliable data channel lane also gained a send-buffer cap (1 MiB by default,
`WebRtcServerConfig::maxReliableBufferedBytes`): a peer past it is dropped, so one that stops
reading cannot grow the server's memory. The limits are flags with these defaults
(`docs/lobby-protocol.md` §7-§8); the deploy units pass `--trusted-proxy 127.0.0.1` and Caddy
sets `X-Forwarded-For` itself.
Lobby protocol v1.3, part two (v0.70): the lobby stops being a liveness dependency of running
matches (correcting v0.37's "a server that drops off the socket leaves the registry and its
rooms close", which still holds at the lobby but no longer ends the match). The lobby admits
players and relays the signaling that opens their data channels; after that a match needs only
the client and its server, yet a lobby restart, deploy or blip used to end every match in every
region, because `LobbyLink` closed every peer with its socket and the client transport ended
the session with its lobby socket. Now `LobbyLink` keeps the peers whose data channels are open
(detached: no lobby peer id) and the room, retries every 1 s doubling to 5 s while it holds
one, and claims the room back in its next `register` (`room`, `tokens`: every token announced
for the room, oldest first, at most 128, and `players`). A v1.3 lobby adopts the claim unless
the code is live there (only if it was reassigned meanwhile) or the room cap is reached, and
says so with `room` in `registered`; the room is then live again under the same code with the
tokens re-announced, so a join by code and the v1.2 rejoin both work after a restart and
Reconnect needs no fallback. Between the lobby coming back and the claim landing (the server
redials after 1, 2, 4, then every 5 s) the code is not live and a rejoin is refused
`unknown_code`; the client retries that refusal for 8 s, which covers the server's longest
gap (v0.76). A claim that is not adopted (that, or a v1.2 lobby, which ignores
the keys) leaves the kept players playing while the server offers capacity 0 until they have
gone. On the client, once both data channels are open the lobby socket's close and a `closed`
`server_gone` end nothing; `kicked`, `replaced` and `room_closed` still label the session's end
for the UI, and the client never redials the lobby (the server's `players` counts it). The
lobby's state stays memory-only: the servers are its source of truth for live rooms.
Alternatives not taken: persisting the lobby's state (a database for a few hundred bytes per
room, and a second source of truth that can disagree with the servers'), and clients
re-seating themselves on a restarted lobby (a browser change and a reconnect storm of every
player at once, where one claim per server suffices). What is still unavailable while the
lobby is down: creating, joining and rejoining. Deploys still end matches, because
`update.sh` restarts the server processes themselves.

### 13.6 Sequencing

1. Web first: the browser is the client that ships. Native stays the development build.
2. Measure. If a native-grade latency floor or revenue becomes important, add the Steam build
   through the two seams; nothing below them changes.

---

## Appendix A: core type sketches

```cpp
// core/fixed.hpp
struct Fixed {
  int32_t raw;
  static constexpr int kShift = 16;
  constexpr friend Fixed operator*(Fixed a, Fixed b) {
    return Fixed{static_cast<int32_t>((int64_t{a.raw} * b.raw) >> kShift)};
  }
  // +, -, /, comparisons, fromInt, toFloat (presentation only), sqrt, sin/cos via LUT
};
struct Vec2 { Fixed x, y; };

// core/pool.hpp
template <class T, uint16_t N>
struct Pool {
  std::array<T, N>        items;
  std::array<uint16_t, N> generation;
  std::bitset<N>          alive;
  EntityId  spawn();                       // returns invalid id when full
  void      despawn(EntityId);
  T*        get(EntityId);                 // nullptr if stale
  template <class F> void forEachAlive(F&&);   // index order, deterministic
};

// sim/step.hpp
void step(WorldState&, const InputFrame&, const ContentDB&, EventSink&);
uint32_t checksum(const WorldState&);         // via visitor, not raw bytes

// sim/abilities/registry.hpp
struct AbilityBehavior {
  bool (*canActivate)(const WorldState&, const Player&, const AbilityDef&);
  void (*onActivate)(WorldState&, Player&, AbilityState&, const AbilityDef&, const ContentDB&,
                     EventSink&);
  void (*onTick)(WorldState&, Player&, AbilityState&, const AbilityDef&, const ContentDB&,
                 EventSink&);
  void (*onPhaseChange)(WorldState&, Player&, AbilityState&, const AbilityDef&,
                        AbilityPhase from, AbilityPhase to, const ContentDB&, EventSink&);
  void (*onEnd)(WorldState&, Player&, AbilityState&, const AbilityDef&, const ContentDB&,
                EventSink&);
};
const AbilityBehavior& behaviorFor(AbilityKind);   // constexpr table indexed by kind

// sim/rules/interaction.hpp
struct InteractionRule {
  uint8_t  priority;
  EntityKind kindA; Tags requireA; Tags forbidA;
  EntityKind kindB; Tags requireB; Tags forbidB;
  uint8_t    phaseMaskB;                   // BoomerangPhase bits when B is a boomerang (0 = any)
  Ownership  ownership;                    // Any, Own, NotOwn: A vs. the owner of B
  uint8_t    relationB;                    // relationBit(Self/Ally/Enemy) of B (or B's owner)
                                           // to A, via MatchContent::relation (v0.29; v0.32:
                                           // kRelationAny by default, the Ally twins need
                                           // FriendlyFire)
  // false = did not apply, keep scanning (a mistimed catch falls through to damage)
  bool     (*resolve)(WorldState&, const ContentDB&, EntityId a, EntityId b, const Contact&,
                      EventSink&);
};
std::span<const InteractionRule> interactionRules();  // sorted by priority

// sim/modes/registry.hpp (v0.23, v0.29)
struct ModeBehavior {
  void (*onRoundStart)(WorldState&, const MatchContent&, EventSink&);
  void (*onPlaying)(WorldState&, const MatchContent&, EventSink&);
  void (*onEliminated)(WorldState&, const MatchContent&, PlayerId victim, PlayerId killer,
                       EventSink&);
  void (*onPickupTaken)(WorldState&, const MatchContent&, PlayerId, const Pickup&, EventSink&);
};
const ModeBehavior& modeBehaviorFor(ModeKind);        // constexpr table indexed by kind

// sim/spaces/registry.hpp (v0.29; as built v0.34, rows Continuous and Grid)
struct SpaceBehavior {
  void (*latch)(WorldState&, const MatchContent&, const InputFrame&);  // InputSystem; grid: once per turn
  void (*move)(WorldState&, const MatchContent&);                       // MovementSystem
};
const SpaceBehavior& spaceBehaviorFor(Space);          // Continuous, Grid

// ai/vision.hpp (v0.35): fog, a projection of one world for one monkey
struct Sight { uint16_t players; uint64_t bananas; uint32_t pickups; };  // by pool index
Sight sightOf(const WorldState&, const MatchContent&, PlayerId monkey);   // its own, one tick
Sight visionOf(const WorldState&, const MatchContent&, PlayerId observer, // own | team | all
               const std::array<Sight, kMaxPlayers>* own = nullptr);
bool inCone(int32_t dc, int32_t dr, uint8_t octant, int32_t range);       // R² + 2R cells
bool lineOfSight(const ArenaDef&, Cell from, Cell to);                    // Bresenham, walls
struct TurnClock { uint16_t turnTicks, ticksToLatch; };
TurnClock turnClock(const WorldState&, const MatchContent&);

// ai/agent_host.hpp (v0.35 additions)
enum class RespawnMemory : uint8_t { Keep, Wipe };   // memory_on_respawn
void AgentHost::fill(InputFrame&, const WorldState&, const MatchContent&);  // window, pause
void AgentHost::observe(const EventSink&, const WorldState& after, const MatchContent&);  // calls
```

## Appendix B: glossary

- **Tick**: one fixed simulation step (1/60 s).
- **Lead**: how many ticks the client simulates ahead of the server so its inputs arrive in time.
- **Rollback**: restoring a saved state and re-simulating with corrected inputs.
- **Authoritative frame**: the server's final `InputFrame` for a tick.
- **Confirmed tick**: the latest tick for which the client holds the authoritative frame.
- **Misprediction**: a predicted input differing from the authoritative one.
- **Sub-tick timestamp**: the fraction of a tick at which a button edge occurred, carried in the input.
- **Lane**: a transport's unreliable-unordered or reliable-ordered message channel.
- **Bot module**: a WebAssembly module implementing the bot ABI (§8); one instance per slot.
- **Observation**: the versioned POD the host writes into a bot module before each `act`.
- **Fuel**: a bot's per-tick budget in wasm instructions; running out costs that tick's input.
- **Bot delay**: ticks a bot's input is held back to match the human lead in a mixed match.
- **Team**: the slots sharing a `MatchConfig::teams` index; **relation**: Self, Ally or Enemy between two slots.
- **Loadout**: the 2–3 abilities a monkey spawns with, a match axis; a **role** is the loadout a team mode gives a position within a team.
- **Baseline**: the power-up a mode grants every monkey at spawn with no expiry.
- **Space**: how monkeys move, `continuous` or `grid`; bananas are continuous in both.
- **Turn**: `turnTicks` ticks of a grid mode; **latch**: the first tick of a turn, when movement intent becomes a commitment.
- **Commitment**: a grid monkey's target cell for the turn; **contested cell**: two commitments to one cell, resolved at the latch.
- **Cell**: a square of `cellSize` arena units; a monkey rests on cell centres, a banana's cell is the cell of its centre.
- **Cone, peripheral, hearing**: the three parts of a monkey's vision (§8), in cells; **focus**: extra cone for a monkey at rest; **banana sight**: cells around an own banana in flight.
- **Turn window**: the ticks since the last latch over which a host ORs a bot's visibility and collects its heard calls (v0.35).
- **Call**: a one-byte shout heard by every monkey in range, the in-game communication channel.
- **Zone**: an arena box that blocks nothing; modes read who stands in it.
