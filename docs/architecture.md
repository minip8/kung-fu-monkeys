# Kung Fu Monkeys — Architecture Design

Status: proposal v0.4 (2026-11-02). Working title taken from the repo name.

v0.2 changes: 60 Hz tick with sub-tick input timestamps (§5.11); GameNetworkingSockets for native
clients plus a WebRTC transport for a planned browser client (§6.3–6.4); 2.5D pre-rendered sprites (§7).
v0.3 changes: platform strategy for Steam and the web, `IPlatformServices` seam (§13).
v0.4 changes: web-first. The browser is the primary shipping client, WebRTC is the single production
transport, native is the development build, Steam is optional (§6.4, §11, §13).

A top-down online arena brawler inspired by Boomerang Fu. Monkeys throw bananas
that fly like boomerangs and return. Movement, throw and dash are the base verbs,
power-ups are picked up in the arena, and the signature twist is that a perfectly
timed dash onto a flying banana catches it. 2–10 players online, C++ engine,
latency-first netcode, and a simulation that bots and ML agents can drive headlessly.

---

## 1. Goals and constraints

| Requirement | Architectural consequence |
|---|---|
| Fast, precise party brawler (one-hit kills, timing windows) | Fixed 60 Hz tick, exact timing semantics, zero local input delay |
| 2–10 players online, smooth | Server-authoritative, input-synchronised rollback netcode, tiny packets |
| Modular gameplay: dash now, more abilities and twists later | Data-driven definitions, table-driven behaviours, an interaction rules table |
| Engine in C++ | C++20, CMake, small set of battle-tested libraries, strict layering |
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
| Presentation | 2.5D: tilted camera over a strictly 2D sim, pre-rendered 8-direction sprites, y-sorted, shadows mark true positions. SDL3 (window/input/audio) + sokol_gfx sprite batcher, Dear ImGui debug UI | Rendering is a read-only consumer of sim state; sokol_gfx has WebGL2/WebGPU backends for the browser build |
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
    presentation/        renderer, sprites/animation, camera, audio, vfx, ui, interpolation+smoothing
    platform/            SDL3 window/input/audio/gamepads, filesystem, main loop, config,
                         services/ (IPlatformServices: steam, web, null)
  content/               data files: abilities/ powerups/ arenas/ modes/ sprites/ audio/
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
- Forbidden inside `sim`: floating point, wall-clock or OS randomness, pointers in state,
  heap allocation after init, iteration over hash containers, exceptions, RTTI, threads.
- `Rng` lives inside `WorldState` and is seeded per match.
- Content definitions are immutable per match and hashed (`content_hash`); peers must also
  match `build_id`. Both are exchanged at connect.
- Enforcement: a replay corpus is run in CI on Linux, Windows and macOS, Debug and Release;
  per-tick checksums must be identical. This is a merge gate, not a nice-to-have.

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

### 5.3 Tick pipeline

```
step(WorldState& s, const InputFrame& in, EventSink& events):
   1. RulesSystem::preTick      round start/end, spawn waves, pickup spawners
   2. InputSystem               decode PlayerInput → Intent; press-edge detection
   3. AbilitySystem             activation, phase state machines, cooldowns (Dash, Throw, …)
   4. MovementSystem            intent × derived stats → velocity; integrate; arena clamp
   5. ProjectileSystem          boomerang flight: outbound → return steering → grounded
   6. CollisionSystem           broadphase (brute force or spatial hash) + swept narrowphase with time-of-impact, emits Contacts
   7. InteractionSystem         resolves Contacts via the rules table (hit, catch, pickup, bounce)
   8. ModifierSystem            tick timers, expire, recompute derived Stats
   9. LifecycleSystem           eliminations, respawns, despawns
  10. RulesSystem::postTick     win conditions, scoring
  11. s.tick++
```

Systems are stateless free functions; the order is fixed and documented; each reads state and
inputs and writes state and events. Nothing else mutates `WorldState`.

### 5.4 Input model

```cpp
struct PlayerInput {            // 5 bytes on the wire, bit-packed smaller
  uint8_t moveDir;              // 0 = idle, 1..255 = angle (or 9-way digital)
  uint8_t moveMag;              // analog magnitude; 255 for digital input
  uint8_t aimDir;               // 0..255 angle (twin-stick aim)
  uint8_t buttons;              // bit0 Throw, bit1 Dash, bit2 Ability1, bit3 Ability2, bit4 Taunt ...
  uint8_t subTick;              // fraction of the tick at which the latest button edge occurred (1/256 tick ≈ 65 µs)
};
struct InputFrame {
  Tick tick;
  std::array<PlayerInput, kMaxPlayers> inputs;
  PlayerMask present;
};
```

This struct is also the bot and reinforcement-learning action space. Button presses are
edge-detected in `InputSystem`, which makes "repeat last input" a safe prediction for remote
players: a held button never re-fires. `subTick` is explained in §5.11; it only carries meaning
on a tick where a button edge occurred, so repeated-input prediction is unaffected.

### 5.5 Abilities (the modularity core)

- `AbilityDef` (content): `kind`, cooldown, phase durations (startup / active / recovery),
  numeric params, and the `Tags` granted during each phase.
- `AbilityState` (per slot, POD): `defIndex`, `phase`, `phaseTicks`, `cooldownTicks`, a small
  `scratch` union for kind-specific data.
- `AbilityBehavior` registry: a table indexed by `AbilityKind` holding function pointers
  `canActivate`, `onActivate`, `onTick`, `onPhaseChange`, `onEnd`. Behaviours are registered at
  startup. Adding an ability touches a new behaviour file, a content def, one registry line and
  a scenario test.
- Players have N ability slots. Base kit is {Dash, Throw}. Power-ups may override a slot
  (for example a teleport dash) or add a temporary slot.
- Phase tags: Dash's active phase grants `Tag::Dashing`; its first `perfectWindowTicks` also
  grant `Tag::PerfectDash`. Tags are a bitset read by the interaction rules.

### 5.6 Interaction rules (where the twists live)

`CollisionSystem` emits contacts `(a, b, info)`. `InteractionSystem` resolves each by scanning
a prioritised rules table keyed on `(kindA, requiredTagsA, kindB, requiredTagsB)`; the first
match wins.

```
prio  A                          B                              handler
 10   Player[PerfectDash]        Boomerang[InFlight]            CatchBoomerang
 20   Player[Shielded]           Boomerang[InFlight, !own]      BreakShield
 30   Player[]                   Boomerang[InFlight, !own]      EliminatePlayer
 40   Player[]                   Boomerang[Returning, own]      AutoCatch
 50   Player[]                   Pickup[]                       GrantPowerUp
 60   Boomerang[]                Wall                           Bounce
```

A new twist is a new rule and possibly a new tag. Rules are tested in isolation by scripted
scenarios ("spawn banana at x moving v, dash at tick t, expect `BoomerangCaught`").
Whether a perfect dash catches your own banana, or a non-perfect dash grants invulnerability,
are rule-table decisions, not code changes.

### 5.7 Boomerang model

- State: `owner`, `phase` {Outbound, Returning, Grounded}, `pos`, `vel`, `spin`, `ticksAlive`,
  modifiers (size, speed, bounces, pierce).
- Flight: outbound for `outboundTicks` or until max range, then steer toward the owner's
  current position with a max turn rate. Bounces off arena walls with restitution. Owner
  eliminated → Grounded → becomes a pickup.
- Caught by a perfect dash (any player) or by touching its owner while returning → `ammo++`.

### 5.8 Power-ups and modifiers

- `PowerUpDef` (content): visuals, duration, list of grants: `StatModifier{stat, add, mul}`,
  `AbilityOverride{slot, abilityDef}`, `ProjectileModifier{...}`, `Tag` grants.
- `ModifierList` per player (fixed capacity, e.g. 8) with remaining ticks. `ModifierSystem`
  recomputes derived `Stats` each tick as `base × Π mul + Σ add`, iterating in slot order.
- Pickup entities reference a `PowerUpDef` by index and are spawned by arena spawners.

### 5.9 Events

- `SimEvent` is a POD tagged union written to a per-tick buffer: `BoomerangThrown`,
  `BoomerangCaught{catcher, how}`, `PlayerDashed`, `PlayerEliminated{victim, killer}`,
  `PowerUpPicked`, `RoundEnded`, ...
- Consumers: presentation (VFX/SFX), scoring, RL rewards, telemetry.
- Under rollback, presentation keys events by `(tick, hash)`. Events re-emitted during
  re-simulation are deduplicated; events from an abandoned timeline that have not played yet
  are dropped; events for ticks at or before the confirmed tick are final.

### 5.10 Arenas and content

- Arena: static collision shapes (AABBs / convex polygons on a grid), spawn points, pickup
  spawners, hazards. Loaded into an immutable `Arena` struct with a prebuilt spatial hash.
- Content pipeline: TOML or JSON → validated → `ContentDB`; `content_hash` is the xxHash of
  the canonical serialized form. Hot reload in dev builds (restarts the match).

### 5.11 Timing precision: 60 Hz plus sub-tick input timestamps

The dash-catch lives or dies on timing consistency, so the tick quantum matters. Three options:

| Option | Resolution | Cost |
|---|---|---|
| 60 Hz tick | 16.7 ms for everything | baseline |
| 120 Hz tick | 8.3 ms for everything | 2× sim and rollback work; only improves input timing if the client really samples input and steps at 120 Hz in real time (a 60 fps render loop running two steps per frame samples input once, so both steps see the same input) |
| 60 Hz + sub-tick timestamps | ~65 µs for *when a press happened*; 16.7 ms for *when state changes* | one byte per input; a few rules become time-aware |

We take the third. Each `PlayerInput` carries `subTick`: the fraction of the current tick at which
the most recent button edge occurred, derived from the OS event timestamp (SDL3 timestamps events
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
- **Deterministic tie-breaking.** Two monkeys dashing for the same banana in the same tick: the
  earlier `subTick` wins, then the lower player index.
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

Client tick, in order:

```
1. net.pump()                       drain packets: auth frames, acks, snapshots, control
2. reconcile()                      any auth frame != predicted → load state, re-sim to L
3. in = sampleInput()               as late as possible before stepping
4. predicted[L].me = in; sendInputs(L, last 8..16 inputs)
5. saveState(L); step(world, predicted[L], events)
6. L++; presentation.render(interpolate(prev, curr), smoothingOffsets, events)
```

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

### 6.7 Authority and abuse resistance

- The server accepts only inputs, validated for range, rate (≤ tick rate plus slack) and tick
  window. There are no "I hit you" messages; nothing the client says about state is trusted.
- Connection tokens issued by a lobby service so only matched players can connect; the token
  rides in the first reliable message. GNS and WebRTC encrypt in transit. Rate limits per address.
  Message decoders are fuzzed.
- Aim assistance cannot be prevented server-side; acceptable for a party game.

### 6.8 Perceived latency budget (local actions)

- Input is sampled immediately before `step`: ≤ 1 tick of staleness (8 ms average). The press
  time is still known exactly through the sub-tick stamp, so timing judgments do not inherit
  this jitter.
- Prediction applies the input in the same tick.
- Render interpolates between the previous and current tick (≤ 1 tick) or renders the newest
  tick directly when display rate ≥ tick rate. Natively, use a low-latency present mode and avoid
  deep swapchain queues.
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
pixels.

---

## 7. Presentation (`engine/presentation`, `engine/platform`)

The sim is strictly 2D. "2.5D" is a presentation decision: a tilted camera over the arena, with
pre-rendered sprites standing on the ground plane.

- **Projection and sorting.** The ground is drawn with an oblique/isometric-style projection of sim
  `(x, y)`. Sprites are camera-facing quads anchored at the entity's feet and sorted by ground `y`
  (painter's order), with a per-sprite depth value so overlaps resolve consistently. Arena walls
  and props are sprites or tile layers in the same sort.
- **Height is cosmetic.** Banana arcs, dash hops and knockback lift the sprite along the screen's
  up axis, but the sim position never leaves the plane. Every airborne thing casts a shadow blob
  at its true sim position, and the shadow is where the hitbox is. This keeps the catch readable:
  you dash at the shadow.
- **Sprite pipeline.** Characters and bananas are modeled and animated in Blender and
  batch-rendered to sprite sheets: 8 directions (16 if the budget allows) × animation frames,
  with an optional normal-map pass for cheap dynamic lighting and an ID-mask channel for
  per-player palette swaps in the shader. Facing is chosen from aim angle (upper body) or move
  angle. The render script lives in the repo so sheets regenerate from models with one command.
- **Renderer.** sokol_gfx sprite batcher: instanced quads from texture atlases, y-sorted per
  frame, alpha blending, a few render targets for post (bloom, hit flashes, screen shake).
  Backends: OpenGL/Metal/D3D11 on desktop, WebGL2 (WebGPU later) in the browser. SDL3 handles
  window, input, gamepads and audio on every target including Emscripten. SDL_GPU was the other
  candidate; at the time of writing it has no stable browser backend, which decides it.
- **Dear ImGui** dev overlay: net graph, rollback depth histogram, entity inspector, content reload.
- `RenderState` is derived every frame from two sim states (prev, curr), an interpolation alpha,
  smoothing offsets and the event buffer. Presentation never mutates `WorldState`.
- UI (menus, lobby, scoreboard, settings) is separate from gameplay rendering.
- Local multiplayer is first-class: several humans on one client each own a player slot, all
  predicted locally; the server sees them as separate players behind one connection.

Alternative kept open: real-time low-poly 3D models on the same 2D plane (what Boomerang Fu itself
does). No direction sheets and smooth rotation, but it needs a 3D renderer with skinning, lighting
and shadows. Nothing below the presentation layer changes, so this can be revisited after M2 with
the placeholder sprites in hand.

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
  on packet decoders; Google Benchmark for ticks/s and rollback cost.
- Determinism gate in CI: replay corpus → identical per-tick checksums across Linux, Windows,
  macOS, Debug and Release.
- Replays: `seed + match config + content_hash + input stream`. The `replay` app scrubs via
  periodic keyframes. Replays attach to bug reports.
- Observability: Tracy zones; structured logging; net stats (RTT, jitter, loss, rollback depth,
  mispredictions/s) in the debug overlay; server metrics endpoint later.
- Code standards: C++20, warnings as errors, clang-format and clang-tidy in CI, ASan/UBSan
  builds, no exceptions or RTTI inside `sim`, strong typedefs (`Tick`, `PlayerId`, `EntityId`),
  `[[nodiscard]]`, no raw `new`.

---

## 10. Write vs. buy

| Write ourselves | Take off the shelf |
|---|---|
| Sim, pools, fixed-point math, swept collision, abilities and rules framework | SDL3, sokol_gfx, Dear ImGui, stb |
| Message layer, prediction/rollback, clock sync, network emulator | libdatachannel + datachannel-wasm (transport); GameNetworkingSockets only for a Steam build |
| Bots, observation builder, vectorised env | LZ4, xxHash, toml++ or nlohmann-json |
| Replay tooling, content validator, sprite-sheet packer | Catch2, Google Benchmark, Tracy, libFuzzer, nanobind, ONNX Runtime, Blender (sprite pipeline) |

Dependencies via vcpkg manifest; CMake presets for `dev`, `release`, `asan`, `server-headless`, `wasm` (Emscripten).

---

## 11. Roadmap

| Milestone | Scope | Exit criterion |
|---|---|---|
| M0 Skeleton | Repo layout, CMake presets, vcpkg, CI, `core` (`Fixed`, `Vec2`, `Pool`, `BitStream`, `Rng`), Catch2, determinism harness | Green CI on three OSes running an empty replay |
| M1 Headless sim | Players, movement, dash, boomerang flight/return, catch rule, eliminations, FFA round rules, replay record/playback, scenario tests, ticks/s bench | A scripted 10-player replay runs identically everywhere |
| M2 Playable local | SDL3 + sokol_gfx client, 2.5D sprite renderer with placeholder sheets (y-sort, shadows, height), local multiplayer, debug UI, interpolation, first feel tuning of dash and catch windows; Emscripten build of the same client runs in a tab | Two people on one keyboard/gamepads have fun for 10 minutes, natively and in the browser |
| M3 Netcode | Loopback + emulator, `NetServer`/`NetClient`, prediction + rollback, clock sync, snapshots/join, `WebRtcTransport` (libdatachannel natively, datachannel-wasm in the browser), `netsim` with 10 clients, soak tests under loss and jitter | 10 clients at 100 ms RTT, 2% loss, no desyncs, rollback depth bounded; a browser client joins a real server |
| M4 Content | Power-ups, modifiers, more abilities, arenas, modes, lobby UI, audio/VFX | Content added without touching system code |
| M5 Bots & ML | Utility bot, nanobind env, PPO self-play baseline, ONNX agent in server | A trained policy beats the scripted bot |
| M6 Ship on the web | Static hosting + CDN for the client, lobby/signaling service with room codes and connection tokens, dedicated servers in 2–3 regions, telemetry, crash reporting | Send a link, friends are in a match within a minute, no install |
| M7 Steam build (optional) | Steamworks `IPlatformServices`, `GnsTransport` against the Steamworks SDK, SteamPipe from CI, store page | Same game with Steam lobbies and invites; Remote Play Together for local mode |

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
- **Sprite pipeline cost.** 8-direction sheets multiply animation work; keep the model-to-sheet
  render automated from day one.
- Decided: 60 Hz tick with sub-tick input timestamps (§5.11); WebRTC as the single production
  transport (§6.4); 2.5D pre-rendered sprites (§7); web first, native as the dev build, Steam
  optional (§13).
- Open: one-hit kills vs. HP; team modes; WebTransport as a later replacement for WebRTC; mobile
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
void step(WorldState&, const InputFrame&, EventSink&);
uint32_t checksum(const WorldState&);         // via visitor, not raw bytes

// sim/abilities/registry.hpp
struct AbilityBehavior {
  bool (*canActivate)(const WorldState&, const Player&, const AbilityDef&);
  void (*onActivate)(WorldState&, Player&, AbilityState&, const AbilityDef&, EventSink&);
  void (*onTick)(WorldState&, Player&, AbilityState&, const AbilityDef&, EventSink&);
  void (*onPhaseChange)(WorldState&, Player&, AbilityState&, Phase from, Phase to, EventSink&);
  void (*onEnd)(WorldState&, Player&, AbilityState&, const AbilityDef&, EventSink&);
};
const AbilityBehavior& behaviorFor(AbilityKind);   // table built once at startup

// sim/rules/interaction.hpp
struct InteractionRule {
  uint8_t  priority;
  Kind     kindA; Tags requireA; Tags forbidA;
  Kind     kindB; Tags requireB; Tags forbidB;
  void   (*resolve)(WorldState&, EntityId a, EntityId b, const Contact&, EventSink&);
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
