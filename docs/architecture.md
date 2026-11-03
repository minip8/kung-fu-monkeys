# Kung Fu Monkeys — Architecture Design

Status: proposal v0.24 (2026-11-03). Working title taken from the repo name.

v0.2 changes: 60 Hz tick with sub-tick input timestamps (§5.11); GameNetworkingSockets for native
clients plus a WebRTC transport for a planned browser client (§6.3–6.4); 2.5D pre-rendered sprites (§7).
v0.3 changes: platform strategy for Steam and the web, `IPlatformServices` seam (§13).
v0.4 changes: web-first. The browser is the primary shipping client, WebRTC is the single production
transport, native is the development build, Steam is optional (§6.4, §11, §13).
v0.5 changes: C++ standard rationale; C++23 probe added to M0 (§9, §11).
v0.6 changes: M0 landed (§11). Trig table generated and checked in, not computed at build time
(§5.1); Catch2 via vcpkg with a FetchContent fallback pinned to the same version (§10); C++23 probe
status (§9).
v0.7 changes: C++23 probe decided: baseline stays C++20 because the probe fails on the Linux CI
compilers (§9).
v0.8 changes: baseline moved to C++23; Linux CI runs on Ubuntu 26.04 (§1, §2, §9).
v0.9 changes: players have HP, banana damage defaults to a one-hit kill (§5.6, §5.7, §12); M1
content is `constexpr` defs behind `ContentDB`, file loading arrives in M4 (§5.10, §11); M1
collision broadphase is a uniform-grid spatial hash checked against brute force (§5.3).
v0.10 changes: HP semantics pinned: integer hits, add-only modifiers, clamp rule on `maxHp`
change, `HitStun` tag, banana grounds on a non-lethal hit unless piercing, `PlayerHit` event
(§5.6, §5.7, §5.8, §5.9).
v0.11 changes: presentation flipped from pre-rendered 2.5D sprites to real-time low-poly 3D on
the same strictly 2D sim (§2, §7, §10, §11, §12). Height stays cosmetic and the shadow still
marks the hitbox.
v0.12 changes: M1 interfaces pinned (§5.3, §5.5, §5.6, Appendix A). `step()` takes the
`ContentDB` explicitly; the ability and rule tables are `constexpr` (no startup registration);
hooks receive the `ContentDB`; a rule handler returns `bool` ("did not apply, keep scanning") and
rules gain an ownership field and a boomerang-phase mask.
v0.13 changes: M1 landed (§11). Integration decisions: nothing moves and no contact resolves
outside `Playing` (§5.7); the perfect catch ignores an own outbound banana (§5.6); `PickupDef`
holds the pickup radius and bananas spawn clear of the thrower (§5.7, §5.10); the stick is idle
iff `moveMag == 0` (§5.4); rule-table semantics and the `HitStun` tick convention pinned (§5.6);
`PerfectDash` covers the sub-tick end of the window (§5.5); the wall bounce stays in
`ProjectileSystem` (§5.6, §5.7); scripted-input scaffolding and `kfm_replay bench` in place of
Google Benchmark for now (§9); wasm stack size (§12).
v0.14 changes: M1 review fixes. §5.11 tie-break rewritten to match §5.6 and the code (`tHit`,
then press time, then pool index); §5.5 `AbilityState` lists its real fields instead of a scratch
union; `subTick` is the stamp of the latest press edge, releases never move it (§5.4, §5.11);
the snapshot reader rejects out-of-range enums, ids and indices (§5.2); the replay corpus is
defined by a checked-in manifest and re-recorded on regeneration, with no content-hash exemption
(§9); the sim float check also rejects float literals, `fromDouble`/`toFloat`/`toDouble` and
`<cmath>` (§5.1).
v0.15 changes: M2 (playable local client) decisions. Presentation pinned: world basis, fixed
arena camera, interpolation/snap/smoothing constants, cosmetic height curves, facing, effects,
palette and placeholder meshes (§7); GL-only sokol backends with hand-written GLSL and a vendored
glad loader, GL 4.3 core natively (§7, §10); the input sampler's tap, sub-tick and aim semantics
(§5.4); the M2 dependency set and how each is sourced (§10); the client smoke test (§9).
v0.16 changes: a perfect catch needs room below `ammoMax`; at full ammo the catch declines and a
non-own banana hits the dasher (§5.6, §12). Dash attack: dashing into an enemy while holding a
banana damages it, through a new rule row on Player × Player contacts, which `CollisionSystem`
now emits and `InteractionSystem` tries in both orientations; `DashAttackDef` content switch,
`PlayerDashHit` event, a scripted-bot behaviour that exercises it (§5.3, §5.6, §5.9, §5.10,
§5.11, §9, §12).
v0.17 changes: `WebRtcTransport` as built (§6.4): channel labels and reliability, the
server-hosted `ws://` signaling socket and its JSON offer/answer/candidate format, the
callback-queue threading model, size limits, exceptions caught at the `kfm_net_webrtc` boundary;
the libdatachannel 0.24.6 / datachannel-wasm dependency pins, OpenSSL, `KFM_WEBRTC` (§10).
Message layer as built (§6.3): delta-coded inputs, worst-case sizes, checksummed snapshots, LZ4
deferred at 4.7 KB; loopback emulator model and the transport contract suite (§6.4); the
browser network check and the two datachannel-wasm link fixes (§9, §10).
The `InputFrame` present mask is the authoritative roster: `InputSystem` copies it
into `MatchRules::present` every tick and `RulesSystem` despawns absent players at a round start,
so a mid-match joiner plays from the next round (§5.4, §6.6). `NetServer` (§6.6, §6.7): handshake
checks, slot assignment, reconnect reservations (10 s), timeouts (5 s), checksum cadence (every 10
ticks), input validation, a token-bucket rate limit (tick rate + 30 messages/s) and kicks after 32
protocol errors in 10 s; the `ResyncRequest` message (§6.3). `NetClient` (§6.1, §6.2, §5.9, §6.9):
the lead controller law and constants, an anchor state that makes rollbacks deeper than the ring
possible, the event journal, spectator delay and the host call sequence. The client's network
mode (`NetSession`, `--connect`, the net overlay panel, per-tick input windows on the
rate-controlled clock) and when `onRollback` is called (§6.9, §7). Match restart and shutdown
semantics (§6.6), `kfm_server` as the one-match-per-process host (§6.5), `kfm_netsim` and the
soak and network ctests plus the `server-headless` CI job (§9), M3 landed (§11).
v0.18 changes: content is data (§5.10). `engine/content` (`kfm::content`, a new module beside
`net`/`presentation`, used by apps and tests only, §4) parses TOML (toml++ 3.4.0, no exceptions,
§10) from `content/*.toml` into a validated `ContentDB`; fixed-point values are exact decimals
truncated toward zero like `Fixed::fromRatio`, speeds are written per second; defs reference
each other by name and are stored as indices; `defaultContent()` stays as the minimal sim test
fixture and the TOML is a superset of it (the base kit, arena 0 and mode 0 must match). `ContentDB` holds up to 8 named arenas and 8
named modes; `MatchConfig` gains `arena` and `mode` indices (not part of `WorldState`, so
checksums are unchanged), and every system, ability hook and rule handler receives a
`MatchContent` (the DB plus the selected arena and mode). The replay header (version 2), the
corpus manifest, `kfm_replay`/`kfm_server`/`kfm_netsim`/`kfm_render_probe`/`kfm_client`
(`--content`, `--arena`, `--mode`) and the join path (`Welcome`, `Snapshot`, `MatchConfig`
messages, protocol version 2) carry the config (§6.6, §9). Apps load `--content DIR`, else
`content/` next to the executable (copied by the build), else the copy compiled into the binary
(the browser's and the replay gate's source); the native client hot-reloads the content
directory in local play.
v0.19 changes: M4 audio and VFX (§7 "Effects", "Audio"). Event-driven effects grow to 18 kinds
(dash anticipation and afterimage trail, banana trail, perfect-catch sparks, hit sparks,
elimination smoke, pickup sparkle, landing dust, bounce spark, banner pop-in) with the event →
effect and event → sound mapping side by side in `event_fx`; trails are sampled from state;
128 live effects. Procedural `SoundBank` (14 synthesised clips, no audio assets), a 16-voice
`Mixer` with a pull API, the SDL3 `AudioDevice` in `platform` (main-thread queue top-up, silent
without a device, opened on the first input in the browser), `--mute` / `--volume` and the F1
Audio panel. SDL3 is built with ALSA on Linux (vcpkg `alsa` feature, or the FetchContent build
when `libasound2-dev` is present), no new dependency (§10). Tests (§9).
v0.20 changes: power-ups as built (§5.8). `PowerUpDef` (content, `[[powerup]]` in
`content/powerups.toml`): a duration, a visual (palette colour + shape) and up to 4 grants of four
kinds: `StatModifier`, `AbilityOverride`, `ProjectileModifier` (snapshotted onto the banana at the
throw), `Tag` (`shielded`, `invulnerable`). A `ModifierList` entry is now `{powerUp, ticksLeft}`;
`ModifierSystem` drops expired entries, derives stats, swaps overridden ability slots while idle
(cooldown capped at the new def's) and sets granted tags, then counts down, so a power-up picked on
tick T shapes the state after T..T+D−1. Pickups gain `PickupKind::PowerUp` and the arena gains
up to 8 `PickupSpawner`s run at the end of `LifecycleSystem` from `ticksInPhase` and the world RNG
(§5.3). Rules rows 20/24 (the shield absorbs one banana hit or dash attack, staggers the victim,
stops the dash, ends its power-up) and 50/51 (pickups split by kind through a new
`pickupMaskB`) (§5.6). `ShieldBroken` event; `PickupSpawned`/`PickupTaken` carry the pickup kind
and power-up (§5.9). Six power-ups, the `throw_fast` ability and the `dojo_powerups` arena; the
default `dojo` has no spawners, so the default match plays exactly as before; the new state
fields (`Boomerang::speedMul`/`radiusMul`, `Pickup::spawner`, `Player::grantedTags`) are
serialised unconditionally, which changed every world checksum, and the corpus was regenerated
(§5.2). `ModifierSystem` owns the tag bits it granted (`Player::grantedTags`), so ability-phase
and power-up tags compose (§5.8). A
scripted-bot behaviour, the `scripted_4p_powerups_1800t` corpus entry and `kfm_replay stats`
power-up counts (§9). Pickups render per def and the HUD shows power-up timers (§7).
v0.22 changes: arenas with interior statics (§5.10 "Arena statics"). `ArenaDef` (now in
`kfm/sim/content/arena.hpp`) gains up to 32 axis-aligned `Wall{min, max}` and 16
`Hazard{min, max, kind, speedMul}` (`Pit`, `Slow`), visited and hashed; convex polygons stay out
of scope. Players (stick and dash alike) and bananas move through one code path,
`statics::moveCircle`: per-axis swept faces then rounded corners, `Slide` for players (a dash
stops at a wall, its phases run on) and `Bounce` for bananas (restitution, `BoomerangBounced`),
the outer box included, bit-identical to M1 without walls; a throw's spawn offset travels the
same path. No spatial structure for statics: a plain loop over ≤ 32 boxes. A pit is a rules-table
row (prio 60, `Player × Hazard`, `fallIntoPit`: eliminated, killer 0xFF) fed by a new
`EntityKind::Hazard` contact; bananas fly over hazards; a slow patch multiplies move speed in
`ModifierSystem`'s derived stats (§5.3, §5.6–§5.8). The loader reads `walls`/`hazards` and
rejects spawn points overlapping a wall or in a pit. Arenas `pillars`, `corridors`, `island` in
`content/arenas/`, `dojo` unchanged; walls and pits drawn from `RenderState` statics (§7); one
corpus replay per new arena (§9).
v0.23 changes: game modes are content (§5.3, §5.9, §5.10, §6.6). `ModeDef` gains a `kind`
(`rounds`, `deathmatch`, `survival`) and per-kind numbers (`timeLimitTicks`, `scoreToWin`,
`respawnTicks`, `respawnInvulnTicks`); `RulesSystem` keeps the shared phase machine and
dispatches the per-mode decisions through a `ModeBehavior` table indexed by kind (`onPlaying`,
`onEliminated`), like the ability registry. `MatchRules` gains per-player `deaths`,
`respawnTicks` and `invulnTicks`; `ticksInPhase` is the mode clock. Deathmatch respawns go
through `LifecycleSystem` at the spawn point farthest from the other monkeys (world RNG on ties)
with `Tag::Invulnerable` as spawn protection, which the damage and dash-attack rows already
forbid. The visitor serialises the new counters like every other field, so every world
checksum changed and the corpus was regenerated; the rounds mode itself plays exactly as before
(it never sets a counter). New events `ScoreChanged`, `PlayerRespawned`, `TimerWarning`; `content/modes.toml`
ships `rounds`, `deathmatch` and `survival`. Presentation (§7): RespawnBurst, ScorePop and
TimerBanner effects, Respawn, Score and TimerWarning sounds, a per-mode HUD scoreboard and clock.
Team modes stay open (§12).
v0.24 changes: the lobby UI (§5.8, §6.6, §7, §9). `MatchConfig` gains `powerUps`, a per-match
power-up set (bit i = `ContentDB::powerUps[i]`, default `kAllPowerUps`): spawners draw from
their own set intersected with it, and an empty intersection neither spawns nor draws the RNG, so
the default config plays exactly as before. It rides in `MatchContent`, the replay header
(format version 3, a ninth optional manifest column) and the join messages (protocol version 3);
`--powerups all|none|a,b` on `kfm_server`, `kfm_client` and `kfm_replay`.

A top-down online arena brawler inspired by Boomerang Fu. Monkeys throw bananas
that fly like boomerangs and return. Movement, throw and dash are the base verbs,
power-ups are picked up in the arena, and the signature twist is that a perfectly
timed dash onto a flying banana catches it. 2–10 players online, C++ engine,
latency-first netcode, and a simulation that bots and ML agents can drive headlessly.

---

## 1. Goals and constraints

| Requirement | Architectural consequence |
|---|---|
| Fast, precise party brawler (one-hit kills by default, timing windows) | Fixed 60 Hz tick, exact timing semantics, zero local input delay |
| 2–10 players online, smooth | Server-authoritative, input-synchronised rollback netcode, tiny packets |
| Modular gameplay: dash now, more abilities and twists later | Data-driven definitions, table-driven behaviours, an interaction rules table |
| Engine in C++ | C++23, CMake, small set of battle-tested libraries, strict layering |
| Bots and trained models later | Deterministic, headless, allocation-free sim; agents are just input providers; Python bindings |
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
| Simulation | Deterministic, fixed timestep (60 Hz) with sub-tick input timestamps, fixed-point math, plain-old-data (POD) world state with fixed-capacity pools | Rollback by `memcpy`, replays are just inputs, Linux servers, native clients and the browser build agree bit-for-bit, ML runs are reproducible |
| Layering | `core` → `sim` → {`net`, `ai`, `presentation`} → apps. `sim` has no I/O, no platform code, no allocation after init | Same sim binary logic in client, server, replay tool, tests and Python |
| Netcode | Server-authoritative deterministic sim with **input synchronisation**: clients send inputs, server relays authoritative inputs, clients predict everyone and roll back | GGPO-style feel (no local input delay) plus a trusted authority, join-in-progress and resync; bandwidth independent of entity count |
| Transport | WebRTC data channels as the single production transport (libdatachannel natively, the same C++ API via datachannel-wasm in the browser); in-process loopback with network emulation for tests; GameNetworkingSockets only if a Steam build happens. All behind one `ITransport` with an unreliable lane and a reliable lane; our message layer (input frames, redundancy, clock sync) sits above | One transport to harden, built-in encryption, browsers and native clients share a match with no bridging; the game netcode never knows which transport is in use |
| Gameplay modularity | Content defs (abilities, power-ups, arenas, modes) + behaviour registries keyed by kind + a prioritised interaction rules table | New ability or twist = new file + def + registration, no edits to existing systems |
| Presentation | Real-time low-poly 3D over a strictly 2D sim: fixed tilted camera, skinned glTF characters, flat/toon shading, blob shadows mark true positions. SDL3 (window/input/audio) + sokol_gfx, Dear ImGui debug UI | Rendering is a read-only consumer of sim state; smooth facing matches continuous aim; sokol_gfx has WebGL2/WebGPU backends for the browser build |
| Bots / ML | `IAgent` produces `PlayerInput` like a human; headless vectorised env via nanobind; ONNX Runtime inference in C++ | Same action space for humans, scripted bots and policies |
| Tooling | CMake presets, vcpkg, Catch2, Tracy, sanitizers, clang-format/clang-tidy, CI with a cross-OS determinism gate | Determinism is enforced, not hoped for |
| Platforms | Web first: the browser is the shipping client, native is the dev/debug/profiling build, Steam is an optional later add. Platform code is confined to `ITransport` and `IPlatformServices` | Join friction decides a party game; everything below the seams is shared, so Steam stays a cheap option |

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
  replay viewer, test harness, Python training env.
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
    ai/                  IAgent, Observation builder, scripted bots, OnnxAgent
    presentation/        renderer (meshes, skinning, materials), animation, camera, audio, vfx, ui,
                         interpolation+smoothing
    platform/            SDL3 window/input/audio/gamepads, filesystem, main loop, config,
                         services/ (IPlatformServices: steam, web, null)
  content/               data files (*.toml: player, abilities, boomerang, arenas, modes; later
                         power-ups) and source models/ audio/
  apps/
    client/              playable client (also hosts local multiplayer)
    server/              headless dedicated server
    replay/              replay viewer / analyser
    netsim/              N clients + 1 server in one process over an emulated network
  bindings/python/       nanobind module `kfm`, Gymnasium/PettingZoo wrappers, training scripts
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
  Pool<Boomerang, kMaxBoomerangs> boomerangs;      // e.g. 64
  Pool<Pickup,    kMaxPickups>    pickups;         // e.g. 32
  Pool<Hazard,    kMaxHazards>    hazards;
  // Arena geometry is immutable content referenced by index; not part of state.
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
  spawner past `kMaxPickupSpawners`, a banana `pierce` above 1 and `grantedTags` bits above
  `Tag::kCount`.
- Power-up fields (v0.20): a banana's `speedMul` and `radiusMul` (after `pierce`, a plain 0/1
  byte), a pickup's `spawner` and a player's `grantedTags` are visited unconditionally like
  every other field; adding them changed every world checksum and the corpus was regenerated.
  A `ModifierList` visits only its live prefix, as before.
- Mode counters (v0.23): `MatchRules`' per-player `deaths`, `respawnTicks` and `invulnTicks`
  are visited unconditionally after the M1 fields, like every other field. Adding them changed
  every world checksum (the corpus was regenerated); a new state field for a real feature is
  the legitimate case for that, not a reason for a second encoding.

### 5.3 Tick pipeline

```
step(WorldState& s, const InputFrame& in, const ContentDB& content, EventSink& events):
   1. RulesSystem::preTick      round start/end, spawn waves
   2. InputSystem               decode PlayerInput → Intent; press-edge detection
   3. AbilitySystem             activation, phase state machines, cooldowns (Dash, Throw, …)
   4. MovementSystem            intent × derived stats → velocity; integrate sliding along walls; arena clamp
   5. ProjectileSystem          boomerang flight: outbound → return steering → grounded; wall bounces
   6. CollisionSystem           broadphase (uniform-grid spatial hash) + swept narrowphase with time-of-impact, emits Contacts
   7. InteractionSystem         resolves Contacts via the rules table (hit, catch, dash attack, pickup)
   8. ModifierSystem            expire, recompute derived Stats (incl. slow hazards), overrides and tags, tick timers
   9. LifecycleSystem           eliminations, respawns, despawns, power-up spawners (v0.20)
  10. RulesSystem::postTick     win conditions, scoring
  11. s.tick++
```

Broadphase: a uniform grid over the arena bounds, rebuilt every tick by counting sort into
fixed-capacity arrays local to `CollisionSystem` (derived data, never part of `WorldState` or
snapshots). Moving entities are inserted by their swept AABB (start, end, radius). Candidate
pairs are deduplicated and sorted by `(kindA, indexA, kindB, indexB)` before the narrowphase,
so contact order does not depend on cell layout. Contacts are Player × Boomerang,
Player × Pickup and (v0.16) Player × Player; a player pair is listed once with
`indexA < indexB`, both players moving, radii from `PlayerDef::radius`, and its contact point is
the midpoint of the two centres at `tHit` so it reads the same from either side. Player pairs are
purely geometric: every touching pair is a contact whatever its tags (at most 45 with 10
players), and the rules table decides what a touch means. A brute-force all-pairs broadphase stays in
the tests as the oracle: a property test asserts both produce the same pair set. A banana's
contact radius is `BoomerangDef::radius × Boomerang::radiusMul` (v0.20, §5.8).

Power-up spawners (v0.20) run at the end of `LifecycleSystem`, not in `RulesSystem::preTick`:
entity creation and removal already live there (grounded bananas become pickups in the same
place), a pickup spawned at the end of tick T is touchable from T+1 like a grounded banana, and
the spawner timeline is a function of `MatchRules::ticksInPhase` that the post-tick has not yet
advanced. The order of the ten systems is unchanged.
the tests as the oracle: a property test asserts both produce the same pair set.
Statics (v0.22) never enter the grid: walls are resolved while integrating (steps 4 and 5,
§5.10), and after the narrowphase `CollisionSystem` appends one Player × Hazard contact per
hazard box holding a player's centre at the end of the tick (player slot order, then hazard
index), with `tHit` the centre's entry time into the box and `b` the hazard's index.

Systems are stateless free functions; the order is fixed and documented; each reads state and
inputs and writes state and events. Nothing else mutates `WorldState`.

Rules dispatch (v0.23). `RulesSystem` owns the phase machine every mode shares: Lobby →
(`MatchStarted`, round 1) RoundStarting (the present slots spawned at the arena's spawn points,
`roundStartTicks` of countdown) → Playing → RoundEnded (`roundEndTicks`) → next round, or
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

Respawn (`LifecycleSystem`, v0.23) is driven only by the timers: during Playing a dead present slot
with a pending `respawnTicks` counts down before this tick's despawns and respawns at 0, so a
player eliminated on tick T is back on tick T + `respawnTicks`. It takes the spawn point whose
nearest other monkey is farthest away (squared distance in raw fixed point), the world RNG
choosing among equal points (one draw per respawn), and gets `Tag::Invulnerable` with
`invulnTicks = respawnInvulnTicks`, which counts down on the same pass and clears the tag at 0:
protected for the interactions of the next `respawnInvulnTicks` ticks. The damage (30) and
dash-attack (25) rows already forbid `Invulnerable` on the victim; a protected monkey can still
attack and catch. A round start clears every timer.

### 5.4 Input model

```cpp
struct PlayerInput {            // 5 bytes on the wire, bit-packed smaller
  uint8_t moveDir;              // 0..255 angle, 0 = +x (digital input uses 8 of them)
  uint8_t moveMag;              // analog magnitude; 0 = idle, 255 for digital input
  uint8_t aimDir;               // 0..255 angle (twin-stick aim)
  uint8_t buttons;              // bit0 Throw, bit1 Dash, bit2 Ability1, bit3 Ability2, bit4 Taunt ...
  uint8_t subTick;              // stamp of the latest press edge within the tick (1/256 tick ≈ 65 µs)
};
struct InputFrame {
  Tick tick;
  std::array<PlayerInput, kMaxPlayers> inputs;
  PlayerMask present;
};
```

The stick is idle iff `moveMag == 0` (v0.13); `moveDir` is always an angle, so every one of the
256 directions, +x included, is reachable. This struct is also the bot and reinforcement-learning action space. Button presses are
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

### 5.5 Abilities (the modularity core)

- `AbilityDef` (content): `kind`, cooldown, phase durations (startup / active / recovery),
  numeric params, and the `Tags` granted during each phase.
- `AbilityState` (per slot, POD): `defIndex`, `phase`, `phaseTicks`, `cooldownTicks`, and the
  activation record `activatedTick`, `activatedSubTick` (press time, read by the catch rule and
  the tie-break) and `dir` (dash or throw direction captured at activation). There is no
  kind-specific scratch union; a kind that needs more state adds a named field (v0.14).
- `AbilityBehavior` registry: a table indexed by `AbilityKind` holding function pointers
  `canActivate`, `onActivate`, `onTick`, `onPhaseChange`, `onEnd`. The table is a `constexpr`
  array (no startup registration, no mutable static state). Adding an ability touches a new behaviour file, a content def, one registry line and
  a scenario test.
- Players have N ability slots. Base kit is {Dash, Throw}. Power-ups may override a slot
  (for example a teleport dash) or add a temporary slot.
- Phase tags: Dash's active phase grants `Tag::Dashing`; its first `perfectWindowTicks` also
  grant `Tag::PerfectDash`, plus the tick after them when the press had a non-zero `subTick`,
  because the window `[press, press + perfectWindowTicks)` measured from the press instant
  closes inside that tick (§5.11). The tag says "the window may be open this tick"; the catch
  rule judges the exact instant. Tags are a bitset read by the interaction rules.

### 5.6 Interaction rules (where the twists live)

`CollisionSystem` emits contacts `(a, b, info)`. `InteractionSystem` resolves each by scanning
a prioritised rules table keyed on `(kindA, requiredTagsA, kindB, requiredTagsB)`; the first
match whose handler returns `true` wins. A handler returning `false` means "did not apply, keep
scanning": that is how a perfect catch judged outside its time window falls through to
`DamagePlayer` without the catch rule knowing about damage. Contacts are resolved in `tHit`
order (then earlier dash press, then pool index), and a contact whose entity an earlier contact
despawned is skipped. Every row forbids `Tag::Eliminated` on the player: a player eliminated
earlier in the same tick stays in the pool until `LifecycleSystem` and must not be hit, catch
or pick up again.

```
prio  A                          B                              handler
 10   Player[PerfectDash]        Boomerang[InFlight]            CatchBoomerang (declines at ammoMax)
 20   Player[Shielded, !HitStun] Boomerang[InFlight, !own]      ShieldBlocksBanana
 24   Player[Dashing, !HitStun]  Player[Shielded, !HitStun]     ShieldBlocksDash (declines below minAmmo)
 25   Player[Dashing, !HitStun]  Player[!HitStun, !Invulnerable] DashAttack (declines below minAmmo)
 30   Player[!HitStun]           Boomerang[InFlight, !own]      DamagePlayer
 40   Player[]                   Boomerang[Returning, own]      AutoCatch
 50   Player[]                   Pickup[Banana]                 TakePickup (declines at ammoMax)
 51   Player[]                   Pickup[PowerUp]                GrantPowerUp (declines with a full list)
 60   Player[]                   Hazard[Pit]                    FallIntoPit
```

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

Pit (v0.22): `CollisionSystem` emits `Player × Hazard` for every hazard holding a player's
centre at the end of the tick (`EntityKind::Hazard`, `b.index` = the hazard's index in
`content.arena.hazards`, `tHit` = the centre's entry time). Row 60 forbids only `Eliminated`:
no tag (not even `Invulnerable` or a dash) saves a monkey over a pit. `fallIntoPit` declines for
non-pit kinds (a slow patch acts through stats, §5.8) and otherwise sets hp 0 and `Eliminated`
and emits `PlayerEliminated` with killer 0xFF and no `PlayerHit`: nobody is credited. Its
`tHit` orders it among the tick's other contacts, so a banana that reached the monkey before it
stepped over the edge still scores.

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
  ticks) so a piercing banana overlapping the victim across ticks lands once, and emits
  `PlayerEliminated` if `hp <= 0`. Tick convention (v0.13): `hitStunTicks = N` counts the hit
  tick. A hit on tick T protects against contacts on T..T+N−1 (the tag is set in step 7 and
  `ModifierSystem` counts down in step 8 of the same tick), freezes movement on T+1..T+N−1
  (movement on T already ran), and the next hit can land on T+N. A scenario test pins this. The banana grounds at the contact point and becomes a pickup
  unless it has `pierce`, in which case it keeps flying.
- Shield and armour stay distinct: `Shielded` is a tag that negates one hit entirely and is
  consumed (rule 20, and rule 24 for a dash attack); armour is `+maxHp`. A mode or power-up picks
  one or stacks both. As built (v0.20): rows 20 and 24 key on the tags of the rows they pre-empt
  (30 and 25), so a shield breaks only on a hit that would land. The block removes the first
  `ModifierList` entry whose def grants `Shielded` (ending that power-up), clears the tag and
  gives the victim `HitStun` for `hitStunTicks` like a hit, without damage; a blocked banana
  grounds at the contact point even when it pierces; a blocked dash stops (the attacker's dash
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
mistimed dash: a non-own banana in flight reaches row 30 and hits the full-ammo dasher (and
grounds into a pickup), an own returning banana reaches row 40 and is auto-caught (absorbed even
at full ammo, as without a dash). A perfect catch therefore always gains a banana, and the dash
is a defence only for a player who has thrown.

### 5.7 Boomerang model

- State: `owner`, `phase` {Outbound, Returning, Grounded}, `pos`, `vel`, `spin`, `ticksAlive`,
  `damage` (default 1, a one-hit kill against base `maxHp`), `pierce`, and (v0.20) the
  projectile modifiers `speedMul` and `radiusMul` snapshotted at the throw (§5.8): the banana
  flies at `BoomerangDef::speed × speedMul` out and back, and its wall limits and contact radius
  use `BoomerangDef::radius × radiusMul`. Bananas already bounce off every wall without limit, so
  an "extra bounces" modifier has nothing to extend and is not built.
- Flight: outbound for `outboundTicks` or until max range, then steer toward the owner's
  current position with a max turn rate. Bounces off arena walls with restitution. Owner
  eliminated → Grounded → becomes a pickup. Hits a player without `pierce` → Grounded at the
  contact point.
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
    (whole `add` only; the loader rejects a `mul`).
  - `AbilityOverride{slot, ability}`: the slot runs that ability def (same kind as the loadout's,
    so the button mapping holds; validated) while the power-up is live; the last override in
    slot order wins.
  - `Projectile`: `ProjectileModifier{speedMul, radiusMul, damageAdd, pierce}`, each mul in
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
  saturating at 255, `pierce` ors) and stored on the `Boomerang` (`damage`, `pierce`, `speedMul`,
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
  | `piercing_banana` | 10 s | bananas pierce |
  | `triple_banana` | 15 s | `ammo_max` + 2 (tops up 2 bananas) |
  | `shield` | 15 s | `shielded`: absorbs one hit |
  | `fast_hands` | 10 s | throw slot runs `throw_fast` (cooldown 8 instead of 20, startup 1) |

  Fast Hands is an `AbilityOverride` rather than a cooldown stat: a new def needs no new
  `StatKind` and no special case in `AbilitySystem`.

### 5.9 Events

- `SimEvent` is a POD tagged union written to a per-tick buffer: `BoomerangThrown`,
  `BoomerangCaught{catcher, how}`, `PlayerDashed`, `PlayerHit{victim, attacker, damage, hpAfter}`,
  `PlayerDashHit{victim, attacker, hpAfter}` (v0.16: a dash attack; `entity` is the attacking
  player where `PlayerHit` carries the banana, and presentation shows both the same way),
  `PlayerEliminated{victim, killer}`,
  `RoundEnded{winner}`, `MatchEnded{winner}` (an invalid winner is a draw),
  ... v0.23 (modes): `ScoreChanged{player, score}` (a deathmatch elimination scored; round wins
  are carried by `RoundEnded`), `PlayerRespawned{player, entity, spawn point}` (a deathmatch
  respawn or late join; `PlayerSpawned` stays the round-start spawn), `TimerWarning{seconds}`
  (10, 3, 2, 1 s left on the mode clock).
  `RoundEnded`, ...
- v0.20: `PickupSpawned` and `PickupTaken` carry the pickup's `PickupKind` in `a` and, for a
  power-up, its def index in `b` (a banana pickup's payload stays 0/0), which is all a
  `PowerUpPicked` would carry, so there is no separate event. `ShieldBroken{victim, entity = the
  banana or the dashing player, a = attacker slot, b = the power-up whose shield broke}` (§5.6).
  No event marks a power-up's expiry: presentation reads the `ModifierList` timers from state.
- Consumers: presentation (VFX/SFX), scoring, RL rewards, telemetry.
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
  - Hazards: the pit is a rules-table row fed by a `Player × Hazard` contact (§5.3, §5.6); a
    slow patch multiplies move speed in `ModifierSystem` (§5.8). Bananas ignore hazards (a
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
  `[[ability]]`, `[[arena]]`, `[[mode]]`, `[[powerup]]` (v0.20) appended in load order. Keys are the C++ field names in
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
- Names: abilities, arenas, modes and power-ups have a `ContentName` (`std::array<char, 16>`, NUL padded,
  1–15 of `a-z 0-9 _ -`), hashed with the def. Defs reference each other by name in TOML (the
  player's loadout `abilities = ["dash", "throw"]`) and the loader resolves them to indices;
  ability index 0 is the built-in inert `none`. Arena and mode names are how command lines and
  the lobby choose (`ContentDB::findArena` / `findMode`); the sim only sees indices.
- Validation: duplicate names, unresolved references, counts against the capacities
  (`kMaxAbilityDefs` 4, `kMaxArenas` 8, `kMaxModes` 8, `kMaxPowerUps` 16; at least one arena
  and one mode), and
  the properties systems rely on instead of re-checking per tick (`validateContent`, also run
  on `defaultContent()` by a test): the loadout's slot kinds, a dash with no startup and its
  perfect window inside the active phase, arena half extents in (0, 1024], spawn points that
  keep a monkey inside (and, v0.22, clear of walls and out of pits), walls and hazards that are
  non-empty boxes inside the arena, a thrown banana clear of its thrower, `minAmmo >= 1`. Diagnostics are
  `file:line: error: message` lines; a failed load leaves the previous `ContentDB` untouched.
- Selection: a `MatchConfig` carries `arena` and `mode` indices (index 0 is the default, the
  first loaded). They are constant for a match like the content and are not part of
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
  `defaultContent()`, because the struct changes anyway.
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
  `round_start_ticks`, `round_end_ticks`; `rounds_to_win` (rounds, survival);
  `time_limit_ticks` (deathmatch: the match, survival: each round; 0 = no clock); `score_to_win`,
  `respawn_ticks`, `respawn_invuln_ticks` (deathmatch). A kind ignores the numbers it does not
  read (they keep their struct defaults and are hashed like every field). Validation: a known
  kind, `rounds_to_win >= 1`, and in deathmatch `score_to_win >= 1` and `respawn_ticks >= 1`.
  The file ships `rounds` (index 0, the M1 numbers), `deathmatch` (first to 10, 3 min, 2 s
  respawn, 1.5 s protection) and `survival` (first to 3 rounds of at most 60 s). The modes stay
  in one file so their order, and so their indices, is explicit.
- `DashAttackDef` (v0.16): `damage` (1), `minAmmo` (1: a banana in hand), `consumesAmmo`
  (0: kept). Visited for the content hash like every other def; the TOML key is
  `[dash_attack]`. The loader's validation requires `minAmmo >= 1` like the M1 tables did;
  allowing 0 (every dash an attack) is a one-line change there when a mode wants it.
- Power-ups (v0.20, §5.8): `[[powerup]]` with `name`, `duration_ticks` (required, >= 1),
  `color` (palette 0–9), `shape` (`orb` | `cube` | `diamond` | `coin`) and `grants` (required,
  1–4 inline tables): `{ kind = "stat", stat = "move_speed" | "max_hp" | "ammo_max", mul, add }`,
  `{ kind = "ability", slot = "dash" | "throw", ability = "<name>" }`,
  `{ kind = "projectile", speed_mul, radius_mul, damage_add, pierce }`,
  `{ kind = "tag", tag = "shielded" | "invulnerable" }`. An arena's optional `spawners` is a list
  of up to 8 `{ pos = [x, y], power_ups = ["<name>", ...], first_ticks, interval_ticks,
  max_alive }` (`power_ups` absent = every power-up). Ability and power-up names resolve after
  every file is read, so the reference may point into a later file. Validation: unknown names,
  an unknown grant kind or stat, a `mul` or a fractional `add` on an integer stat, an override of
  another kind than its slot's def, a tag a power-up may not grant, multipliers outside (0, 4],
  a spawner outside the arena (by the pickup radius), inside a wall or in a pit (v0.22 statics), spawners without any power-up defined, an
  arena's `max_alive` sum above 16.

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
  replays and ML would need a separate deterministic path anyway.
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
keeps it `spectatorDelayTicks` (3) behind the newest one. `sizeof(NetClient)` is about 360 KB
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
| `Snapshot` | S→C | reliable (transport fragments) | full `WorldState` via visitor, LZ4-compressed, plus tick |
| Control | both | reliable | Hello/Connect (token), ContentHash/BuildId, JoinMatch, PlayerJoined/Left, MatchConfig, Chat, Disconnect |
| Ping | both | transport stats plus our own tick-stamped ping | — |
| `ResyncRequest` (v0.17) | C→S | reliable | tick and client checksum of a mismatch; answered with a `Snapshot` |

Redundancy replaces retransmission on the hot path, so a lost packet costs nothing unless 8+
consecutive packets vanish. Unreliable messages stay under 1200 bytes so they never fragment;
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
- Control: `Hello` (version, `contentHash`, `buildId`, a token of up to 64 bytes, requested slot
  count with 0 = spectate), `Welcome` (assigned slots, `MatchConfig`, server tick), `Reject`
  (protocol, content or build mismatch, bad token, match full, shutting down), `JoinMatch`,
  `PlayerJoined`, `PlayerLeft`, `MatchConfig` (with its start tick), `Disconnect` (quit, kicked,
  timeout, protocol error, shutting down). `Ping{sendNs, tick}` / `Pong{echo, tick, holdNs}`.
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
  start at 1 and are never reused; the receive queue holds 8192 messages.
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
  (`rtc::WebSocketServer`, ephemeral port allowed); the lobby service takes it over later.
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
  multi-match host below is the M6 shape.
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
  protocol version 3, v0.24) is all a client needs to simulate the server's arena, mode and
  power-ups. A client steps with the config of the snapshot it last loaded
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
  so nothing (scores, deaths, respawn timers) carries over. A deathmatch joiner does not wait for
  a next round that never comes: its slot spawns `respawnTicks` after it appears in the frames
  (§5.3), at the farthest spawn point.

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
  at 1×; the time scale, clamped to 0.25–2×, scales the units added per frame); at most 4 ticks
  run per frame (adjustable 1–4) and any further backlog is dropped and counted, never
  simulated late. Inputs for tick i are sampled over the wall-clock window that tick covers,
  immediately before its `step()`; the windows of a frame's batch end at the frame's own
  timestamp (the time up to which events were read) and count back in whole ticks, so the
  leftover accumulator only sets the render alpha and never pushes a press past its tick's
  end. Pausing freezes the accumulator and discards pending presses, and a Throw or Dash
  pressed during the pause and still held reads as released until it is let go, so nothing
  fires on resume or on a single step; a new match, a restart or a return to the lobby clears
  the pause and any pending single step, and a return to the lobby also resets the
  presentation state. Render policy "auto": the newest tick is drawn directly when the
  measured display rate (EMA of frame intervals) is at least 0.95 × `kTickHz` × time scale (the
  tolerance covers 59.94 Hz displays), otherwise frames interpolate with
  `alpha = accumulator / tickCost`; the overlay can force either.
- Browser specifics: request the WebGL context with `desynchronized: true` (bypasses the
  compositor queue in Chromium), read input and step the sim at the top of the
  `requestAnimationFrame` callback, render the newest predicted tick directly rather than
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
  `kArenaPitRimColor`, `kArenaSlowColor` in `palette.hpp`, sizes in `tuning.hpp`.
  `kfm_render_probe --arena NAME` (and the client's `--arena`) shows any arena.
- **Height is cosmetic.** Banana arcs, dash hops and knockback lift the model along world `y`,
  but the sim position never leaves the plane. Every airborne thing casts a blob shadow (a decal
  quad on the ground) at its true sim position, and the shadow is where the hitbox is. This
  keeps the catch readable: you dash at the shadow.
- **Facing.** Characters rotate smoothly. Upper body follows the aim angle, lower body the move
  direction, blended in the animation layer; the throw direction, the aim indicator and the
  body always agree. This is the main readability gain over direction-quantised sprites.
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
  whose Start then reconnects. Online, pause, step, time scale, bot keys, restart and content
  tuning do nothing (the server's clock and content rule; a content edit would break the hash)
  and the overlay says so. The F1 overlay gains the §7 net panel: RTT/jitter/loss with a graph,
  lead against target, arrival margin, clock rate, rollback depth graph and histogram,
  mispredictions and substitutions per second, journal counts, checksums compared/mismatched,
  resyncs, bytes/s each way. Remote humans and server bots both read `P<n>` (the roster carries
  no bot flag). First end-to-end runs: a native client and the browser page each joined a real
  `NetServer` over WebRTC on one machine (RTT about 3 ms in the browser, roughly 60 rollbacks/s
  of depth ≤ 3 against bots, 0 checksum mismatches); the 3 s real-socket client test
  (`kfm_client_net_tests`, label `network`) syncs in about 185 ms.
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
- Palette (slot order): red, blue, yellow, green, orange, purple, cyan, pink, white, charcoal;
  every pair at least 0.25 apart in RGB.
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
  (unlit discs for shadows, rings and bursts). Uniform blocks are flat `vec4` arrays in std140
  layout (matrices as four columns; the C++ structs are built only from `Mat4`/`Vec4` and
  static-asserted to 16·N bytes, so there is no vec3 padding to get wrong); at most 8 vec4s per
  stage, far below WebGL2 limits. One directional light with a three-band toon ramp; a player
  flash mixes toward white, dashing adds emissive, invulnerability blinks. The floor grid is
  `fract(uv)` with `fwidth` anti-aliasing. Blob shadows are alpha-blended discs just above the
  ground with depth test on, depth write off and a depth bias, darker at the rim so the hitbox
  edge reads; a gold ring around the feet shows the `PerfectDash` window, a stretched trail disc
  shows a dash, a small banana in hand shows ammo, flying bananas are tinted toward the owner's
  colour. Draw order: opaque, alpha ground decals and billboards, additive billboards; nothing is
  sorted. The per-frame path allocates nothing. Measured 47–55 draw calls and under 5k triangles
  for 10 players; budget 100 draw calls per frame before instancing is worth doing.
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
| `PlayerEliminated` | EliminationBurst 0.6 s, EliminationPoof 0.7 s (5 smoke puffs), shake | Elimination |
| `PickupTaken` | PickupSparkle 0.35 s at the pickup; a power-up (v0.20) adds PowerUpBurst 0.5 s (a ring and 8 sparks in the taker's colour) | Pickup; PowerUp (rising arpeggio) for a power-up |
| `ShieldBroken` (v0.20) | ShieldShatter 0.4 s (a blue ring, flash and 7 shards), flash, shake | ShieldBreak (glassy crack) |
| `BoomerangGrounded` | GroundDust 0.35 s where it landed | BananaGround |
| `BoomerangBounced` | BounceSpark 0.15 s | WallBounce |
| `RoundStarted`, `RoundEnded`, `MatchEnded` | banners 1 / 1.5 / 3 s; the HUD text pops in at 1.35× and settles over the first quarter (`bannerScale`) | — , RoundEnd, MatchEnd |
| `PlayerRespawned` (v0.23) | RespawnBurst 0.6 s at the spawn point (a ring collapsing in the slot colour and a rising glow); the spawn protection then blinks the body (the `Invulnerable` flag) | Respawn (rising shimmer) |
| `ScoreChanged` (v0.23) | ScorePop 0.5 s (gold ring at the scorer) | Score (coin blip) |
| `TimerWarning` (v0.23) | TimerBanner 0.8 s: the HUD clock pops | TimerWarning (beep) |

Sampled from state, not events: a player whose Dash is active leaves a DashTrail afterimage
(0.18 s) every 25 ms, a flying banana a BananaTrail puff (0.12 s, tinted toward the owner) every
30 ms; the count-in sound plays on entering `RoundStarting` and at every whole second the
countdown crosses, RoundGo on `RoundStarting` → `Playing`. Entity positions for events come from
`curr`, else `prev` (despawned this tick), else the entity's presentation track. Player colours
come from `palette.hpp`. Bursts cost one draw call per spark; a busy 10-player frame measured
72–90 draw calls.

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
- Not yet: music, distance attenuation, per-player local-vs-remote mixing and saving the settings
  between runs (they live in `ClientOptions` for the run); no event exists yet for power-ups (M4
  power-ups add theirs to the table).

Character drawing sits behind one interface in `presentation` so that nothing else knows
whether a character is a skinned mesh or a quad. The earlier plan, pre-rendered 8-direction
sprites from the same Blender models, remains the fallback if the 3D renderer stalls M2: the
art is identical up to the export step, and nothing below the presentation layer changes.

---

## 8. Bots and machine learning (`engine/ai`, `bindings/python`)

- `IAgent { PlayerInput act(const Observation&, Tick); void onEvents(span<const SimEvent>); }`.
  Humans, scripted bots, ONNX policies and Python policies all produce `PlayerInput`. Agents
  live outside the sim: server-side bots write straight into the authoritative frame (no
  network); client-side practice bots feed the local input path.
- `Observation` builder in C++: egocentric features (self position, velocity, cooldowns, ammo,
  modifiers; nearest K players; nearest K boomerangs with position, velocity, phase, owner
  relation, time-to-impact; pickups; arena raycasts or a coarse occupancy grid). Versioned
  schema. The full `WorldState` is also exposed for custom encoders.
- Scripted bots: utility AI (dodge / approach / throw / dash-catch scorers) with reaction delay
  and aim noise for difficulty tiers. They double as RL opponents and as a netcode load tool.
- Training: `kfm` Python module (nanobind). `VecEnv(num_envs, players_per_env, seed)` with
  `reset()` and `step(actions) → obs, rewards, dones, infos` as numpy arrays; worlds are
  stepped in parallel in C++; `action_repeat` (act every 2–4 ticks); rewards computed from
  `SimEvent`s with shaping hooks; PettingZoo-parallel wrapper for self-play. Headless target:
  well over 100k ticks/s per core.
- Imitation data comes for free: every online match is already an input replay.
- Deployment: export to ONNX; `OnnxAgent` (ONNX Runtime, CPU) runs inside the server. Bots can
  be given artificial input delay to match human online conditions.

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
  `webglContextAttributes` (`desynchronized`, `powerPreference`, no antialias) and, because SDL3
  creates its context through `emscripten_webgl_create_context` which ignores that setting,
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
  the mirror cannot drift from the sim, and no instrumentation lives in `engine/sim`.
- Browser check (v0.15): `tools/web/serve.py` serves the wasm build (`--page`, optional
  COOP/COEP headers) and `tools/web/check_browser.py` drives it in headless Chromium through
  Playwright (page loads, no console errors, WebGL2 context attributes, frame intervals,
  keyboard input, screenshots). It is a developer tool, not a CI step: a browser download per
  run and timed waits would make the gate slow and flaky, and `client_smoke` already covers
  rendering on CI.
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
  extra arenas and modes). v0.22 adds one 4-player scripted replay per walled arena
  (`scripted_4p_pillars_1800t`, `scripted_4p_corridors_1800t`, `scripted_4p_island_1800t`), so
  wall slides, bounces, pit falls and slow patches are under the cross-platform gate. The
  embedded content depends on the globbed file list as well as the files (a list file rewritten
  only when it changes), so adding, moving or deleting a TOML file re-embeds before a regen.
  v0.24: replays (format version 3) store the per-match power-up set; a ninth optional column
  names it (`all`, `none` or comma-separated names, default all), and
  `scripted_4p_powerset_1800t` (dojo_powerups with only shield and big_banana) keeps the
  intersection under the gate.
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
- Replays: `seed + match config + content_hash + input stream`. The `replay` app scrubs via
  periodic keyframes. Replays attach to bug reports.
- Observability: Tracy zones; structured logging; net stats (RTT, jitter, loss, rollback depth,
  mispredictions/s) in the debug overlay; server metrics endpoint later.
- Code standards: C++23, warnings as errors, clang-format and clang-tidy in CI, ASan/UBSan
  builds, no exceptions or RTTI inside `sim`, strong typedefs (`Tick`, `PlayerId`, `EntityId`),
  `[[nodiscard]]`, no raw `new`.
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
  GCC ≥ 14 or Clang ≥ 19; the dedicated server's deployment is decoupled from the host distro by
  building in CI and shipping the binary (static libstdc++) or a container, so this is a
  build-machine constraint, not a hosting one. The probe stays in the build as a canary.

---

## 10. Write vs. buy

| Write ourselves | Take off the shelf |
|---|---|
| Sim, pools, fixed-point math, swept collision, abilities and rules framework | SDL3, sokol_gfx, Dear ImGui, stb |
| Message layer, prediction/rollback, clock sync, network emulator | libdatachannel + datachannel-wasm (transport); GameNetworkingSockets only for a Steam build |
| Bots, observation builder, vectorised env | LZ4, xxHash, toml++ or nlohmann-json |
| Replay tooling, content validator, glTF export script | Catch2, Google Benchmark, Tracy, libFuzzer, nanobind, ONNX Runtime, cgltf, Blender (asset pipeline) |

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

---

## 11. Roadmap

| Milestone | Scope | Exit criterion |
|---|---|---|
| M0 Skeleton | Repo layout, CMake presets, vcpkg, CI, `core` (`Fixed`, `Vec2`, `Pool`, `BitStream`, `Rng`), Catch2, determinism harness, C++23 toolchain probe (§9) | Green CI on three OSes plus wasm running an empty replay; C++20 vs C++23 baseline decided |
| M1 Headless sim | Players, movement, dash, boomerang flight/return, catch rule, eliminations, FFA round rules, replay record/playback, scenario tests, ticks/s bench | A scripted 10-player replay runs identically everywhere |
| M2 Playable local | SDL3 + sokol_gfx client, 3D renderer with placeholder meshes (capsule monkeys, fixed camera, blob shadows, cosmetic height, GPU skinning on one rigged test model), local multiplayer, debug UI, interpolation, first feel tuning of dash and catch windows; Emscripten build of the same client runs in a tab | Two people on one keyboard/gamepads have fun for 10 minutes, natively and in the browser |
| M3 Netcode | Loopback + emulator, `NetServer`/`NetClient`, prediction + rollback, clock sync, snapshots/join, `WebRtcTransport` (libdatachannel natively, datachannel-wasm in the browser), `netsim` with 10 clients, soak tests under loss and jitter | 10 clients at 100 ms RTT, 2% loss, no desyncs, rollback depth bounded; a browser client joins a real server |
| M4 Content | Power-ups, modifiers, more abilities, arenas, modes, lobby UI, audio/VFX | Content added without touching system code |
| M5 Bots & ML | Utility bot, nanobind env, PPO self-play baseline, ONNX agent in server | A trained policy beats the scripted bot |
| M6 Ship on the web | Static hosting + CDN for the client, lobby/signaling service with room codes and connection tokens, dedicated servers in 2–3 regions, telemetry, crash reporting | Send a link, friends are in a match within a minute, no install |
| M7 Steam build (optional) | Steamworks `IPlatformServices`, `GnsTransport` against the Steamworks SDK, SteamPipe from CI, store page | Same game with Steam lobbies and invites; Remote Play Together for local mode |

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
match config, multi-match hosting (§6.5), a flush signal on the reliable lane for shutdown, and
a human playtest over a real link.

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
- **Visual noise from predicting 9 remote players.** Mitigate with smoothing, the optional
  1–2 tick input delay, and, if needed, rendering remote players 1–2 ticks behind confirmed.
- **Catch feel under latency.** Catcher-favouring is inherent; the thrower sees the catch
  ≈ RTT late. Add anticipation VFX on the dash so the result reads as intentional (v0.19: the
  DashAnticipation ring and flash at the press, §7).
- **Snapshot growth** if entity caps rise; fine up to ~100 KB with LZ4. Measured (v0.17): at
  most 4.7 KB uncompressed with every pool full, so LZ4 is deferred until a cap rises (§6.3).
- **Lobby/matchmaking/NAT** is a service problem, deferred behind the connection-token boundary.
- **Browser as the primary client.** One to two frames more input latency than a tuned native
  client, Safari quirks (audio gesture, gamepad, WebGL), tab throttling (covered by snapshot
  resync), no threads without COOP/COEP headers (the sim is single-threaded anyway), WebGL2 as
  the floor. Fixed-point determinism holds in WebAssembly for free, which floats would not.
- **Always-on servers.** The web needs dedicated servers and a signaling service running 24/7.
  The sim is cheap enough that one small VPS per region hosts hundreds of matches, but it is an
  ongoing cost and an ops duty that a Steam peer-to-peer game would not have.
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
- Open: team modes (v0.23 leaves them out: they need a team per slot in `MatchConfig`, team
  scoring in a `ModeBehavior` and a friendly-fire decision in the rules table; the mode table
  and the per-player mode counters are the seams); WebTransport as a later replacement for WebRTC;
  mobile browsers and touch controls.

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
sanitizers, `netsim` with ten clients, the replay viewer and the ML environment all live there,
and it is the reference the browser build is checked against. Shipping it without Steam adds
installers for three OSes, Windows SmartScreen and macOS notarisation (paid certificates),
auto-update and crash reporting, and buys one to two frames of latency and no audience. For
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
`null` implementation serves tests and the headless server.

### 13.5 Hosting: dedicated servers, decided

Browsers can host a listen server over WebRTC, but the host tab gets throttled, the host has zero
lead, and host migration is real work. Dedicated servers are the cleaner answer and the
architecture already assumes them: clients connect out, so no TURN relay is needed and NAT is a
non-issue; the server is the authority, the bot host and the ML host; a match costs ~100 KB/s and
a sliver of a core, so one small VPS per region (EU, NA, Asia to start) hosts hundreds of matches.
The lobby/signaling service is a small WebSocket endpoint that issues room codes and connection
tokens and tells the browser which server to dial.

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
  // false = did not apply, keep scanning (a mistimed catch falls through to damage)
  bool     (*resolve)(WorldState&, const ContentDB&, EntityId a, EntityId b, const Contact&,
                      EventSink&);
};
std::span<const InteractionRule> interactionRules();  // sorted by priority
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
