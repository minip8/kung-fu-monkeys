# Kung Fu Monkeys — Architecture Design

Status: proposal v0.17 (2026-11-03). Working title taken from the repo name.

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
v0.17 changes: the `InputFrame` present mask is the authoritative roster: `InputSystem` copies it
into `MatchRules::present` every tick and `RulesSystem` despawns absent players at a round start,
so a mid-match joiner plays from the next round (§5.4, §6.6). `NetServer` (§6.6, §6.7): handshake
checks, slot assignment, reconnect reservations (10 s), timeouts (5 s), checksum cadence (every 10
ticks), input validation, a token-bucket rate limit (tick rate + 30 messages/s) and kicks after 32
protocol errors in 10 s; the `ResyncRequest` message (§6.3). `NetClient` (§6.1, §6.2, §5.9, §6.9):
the lead controller law and constants, an anchor state that makes rollbacks deeper than the ring
possible, the event journal, spectator delay and the host call sequence.

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
    sim/                 WorldState, step(), systems/, abilities/, rules/, content/, events,
                         serialization (visitor), checksum
    net/                 messages/ (bit-packed frames, redundancy), transport/ (webrtc, loopback+emulator, optional gns),
                         clock sync, NetClient, NetServer, snapshot codec
    ai/                  IAgent, Observation builder, scripted bots, OnnxAgent
    presentation/        renderer (meshes, skinning, materials), animation, camera, audio, vfx, ui,
                         interpolation+smoothing
    platform/            SDL3 window/input/audio/gamepads, filesystem, main loop, config,
                         services/ (IPlatformServices: steam, web, null)
  content/               data files: abilities/ powerups/ arenas/ modes/ models/ audio/
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
- Apps wire everything together. Only apps and `platform` touch the OS.

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
  system would index or shift by.

### 5.3 Tick pipeline

```
step(WorldState& s, const InputFrame& in, const ContentDB& content, EventSink& events):
   1. RulesSystem::preTick      round start/end, spawn waves, pickup spawners
   2. InputSystem               decode PlayerInput → Intent; press-edge detection
   3. AbilitySystem             activation, phase state machines, cooldowns (Dash, Throw, …)
   4. MovementSystem            intent × derived stats → velocity; integrate; arena clamp
   5. ProjectileSystem          boomerang flight: outbound → return steering → grounded
   6. CollisionSystem           broadphase (uniform-grid spatial hash) + swept narrowphase with time-of-impact, emits Contacts
   7. InteractionSystem         resolves Contacts via the rules table (hit, catch, dash attack, pickup)
   8. ModifierSystem            tick timers, expire, recompute derived Stats
   9. LifecycleSystem           eliminations, respawns, despawns
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
the tests as the oracle: a property test asserts both produce the same pair set.

Systems are stateless free functions; the order is fixed and documented; each reads state and
inputs and writes state and events. Nothing else mutates `WorldState`.

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
leaves mid-round keeps its idle monkey until it is eliminated or the round ends. The server
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
 20   Player[Shielded]           Boomerang[InFlight, !own]      BreakShield
 25   Player[Dashing, !HitStun]  Player[!HitStun, !Invulnerable] DashAttack (declines below minAmmo)
 30   Player[!HitStun]           Boomerang[InFlight, !own]      DamagePlayer
 40   Player[]                   Boomerang[Returning, own]      AutoCatch
 50   Player[]                   Pickup[]                       GrantPowerUp
 60   Boomerang[]                Wall                           Bounce
```

M1 ships rows 10/11 (the catch, see below), 30, 40 and 50 (`takePickup`: a grounded banana
restores one ammo and stays put at `ammoMax`); v0.16 adds row 25 (the dash attack, below). Rows
20 and 60 do not exist yet. The wall bounce lives in `ProjectileSystem` (§5.7), not in the table:
the arena is a box, the bounce is a per-axis reflection done while integrating, and
`CollisionSystem` emits only Player × Boomerang, Player × Pickup and Player × Player contacts. A
Wall row arrives when arenas gain interior shapes and some banana kinds should not bounce, at
which point walls become contact sources.

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
  consumed (rule 20); armour is `+maxHp`. A mode or power-up picks one or stacks both.
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
  `damage` (default 1, a one-hit kill against base `maxHp`), modifiers (size, speed, bounces,
  pierce).
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
- Pickup entities reference a `PowerUpDef` by index and are spawned by arena spawners.

### 5.9 Events

- `SimEvent` is a POD tagged union written to a per-tick buffer: `BoomerangThrown`,
  `BoomerangCaught{catcher, how}`, `PlayerDashed`, `PlayerHit{victim, attacker, damage, hpAfter}`,
  `PlayerDashHit{victim, attacker, hpAfter}` (v0.16: a dash attack; `entity` is the attacking
  player where `PlayerHit` carries the banana, and presentation shows both the same way),
  `PlayerEliminated{victim, killer}`,
  `PowerUpPicked`, `RoundEnded`, ...
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

- Arena: static collision shapes (AABBs / convex polygons on a grid), spawn points, pickup
  spawners, hazards. Loaded into an immutable `Arena` struct with a prebuilt spatial hash.
- Content pipeline: TOML or JSON → validated → `ContentDB`; `content_hash` is the xxHash of
  the canonical serialized form. Hot reload in dev builds (restarts the match).
- Staging: in M1 the defs (Dash, Throw, banana, one box arena, FFA rules) are `constexpr`
  tables in `engine/sim`, assembled into the same immutable `ContentDB` and hashed through the
  visitor, so `content_hash` exists from M1 on. M4 replaces the `constexpr` source with the
  TOML/JSON loader without touching systems. Every number a system needs lives there, including
  the ones that are easy to forget: the pickup contact radius is `PickupDef::radius` (0.5),
  not the banana's flight radius (0.35), so a grounded banana is easier to walk over than to dodge.
- `DashAttackDef` (v0.16): `damage` (1), `minAmmo` (1: a banana in hand), `consumesAmmo`
  (0: kept). Visited for the content hash like every other def; M4's loader exposes it, and
  `minAmmo = 0` (every dash an attack) is a valid content choice there, while the default content
  asserts `minAmmo >= 1`.

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

- Server: `MatchInstance` = world + `NetServer` + bots. A `MatchHost` process runs many
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

---

## 7. Presentation (`engine/presentation`, `engine/platform`)

The sim is strictly 2D. Presentation draws it as real-time low-poly 3D: a fixed tilted camera
over the arena, skinned character models standing on the ground plane. Sim `(x, y)` maps to
world `(x, z)`; world `y` is height and is never read back into the sim.

- **Camera and projection.** One fixed camera per arena (perspective with a long lens, or
  orthographic; tuned in M2), looking down at the plane. No camera rotation in gameplay, so the
  tilt is part of the arena's design. A real depth buffer resolves overlaps; only transparent
  effects are sorted.
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
  windows. The browser page turns its query string into the same flags
  (`?autostart&bots=3&seed=7&overlay`).

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
  banners 1 / 1.5 / 3 s; at most 64 live effects, the one nearest expiry is replaced when full.
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
- Determinism gate in CI: replay corpus → identical per-tick checksums across Linux, Windows,
  macOS, Debug and Release. A replay carries the content hash and `kfm_replay` refuses one whose
  hash differs from the build's (no legacy exemption), so the corpus is defined by a checked-in
  manifest, `tools/replay-corpus/corpus.txt` (name, kind, seed, input seed, players, ticks), and
  `regen-replay-corpus` re-records every entry before regenerating its `.checksums` (v0.14). A
  content or gameplay change regenerates the corpus in the same commit; a `.kfmr` without a
  manifest line fails the regeneration.
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
enabled optional dependency is missing, and neither path has Linux audio until the `alsa`
feature is added); Dear ImGui 1.93.0 (FetchContent only: the vcpkg `imgui[sdl3-binding]` port
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
  ≈ RTT late. Add anticipation VFX on the dash so the result reads as intentional.
- **Snapshot growth** if entity caps rise; fine up to ~100 KB with LZ4.
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
- Open: team modes; WebTransport as a later replacement for WebRTC; mobile
  browsers and touch controls.

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
