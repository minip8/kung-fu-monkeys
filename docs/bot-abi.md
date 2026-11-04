# Kung Fu Monkeys — Bot ABI

Status: normative, ABI and `Observation` version 1 (2026-11-04). The design and its rationale are
in `docs/architecture.md` §8; this document is what an SDK author implements against. The C++
source of truth is `engine/ai/include/kfm/ai/observation.hpp`; a unit test
(`tests/unit/ai/observation_test.cpp`) checks offsets from the tables below against it, and the
header `static_assert`s every block's size and key offsets. Where this document and the header
disagree, the header is right and this document has a bug.

## 1. The module

A bot is one core WebAssembly module (no WASI, no threads, no SIMD requirement). The host runs one
instance per monkey, each with its own linear memory and its own budget, even when several
monkeys run the same module. A module is a pure function of its seed and the observations it is
given: it has no clock, no randomness beyond the seed and no I/O, so a match is reproducible from
the seed, the match configuration, the content hash and the modules.

### 1.1 Exports (all four required)

| Export | Wasm signature | Meaning |
|---|---|---|
| `kfm_abi_version` | `() -> i32` | The `Observation` schema version the module was built against, as a u32. Must return `1`. The host refuses to load a module that returns anything else. |
| `kfm_obs_buffer` | `() -> i32` | Address in the module's linear memory of a buffer of at least `sizeof(Observation)` = 5748 bytes, 4-byte aligned. The host writes the observation there before every `kfm_act`. The address must stay the same for the life of the instance. |
| `kfm_init` | `(seed: i64, slot: i32) -> ()` | Called once before the first `kfm_act` (and again when the host wipes the bot's memory on respawn, a v0.29 host knob). `seed` is a u64 the host derives from the match seed; `slot` is the monkey's player slot, 0..9. |
| `kfm_act` | `(tick: i32) -> i64` | The input for sim tick `tick` (a u32, equal to `match.tick` of the observation just written), packed as below. |

Call order per instance: instantiate, `kfm_abi_version`, `kfm_obs_buffer`, `kfm_init`, then once
per tick the host writes the observation into the buffer and calls `kfm_act`. A module must not
assume it is called on every tick of the match: a host may skip ticks (a slot that is not in the
roster, a paused eliminated monkey), and `match.tick` says which tick an observation is for.

### 1.2 The action: `kfm_act`'s return value

The low 40 bits are the 5-byte `PlayerInput` (architecture §5.4), little-endian; bits 40..63 must
be zero (reserved for a later ABI version).

| Bits | Byte | Field | Meaning |
|---|---|---|---|
| 0..7 | 0 | `moveDir` | stick direction, 1/256 turn, 0 = +x, counter-clockwise toward +y; ignored when `moveMag` is 0 |
| 8..15 | 1 | `moveMag` | stick magnitude, 0 = idle, 255 = full |
| 16..23 | 2 | `aimDir` | aim, 1/256 turn; the body faces it, a throw goes along it |
| 24..31 | 3 | `buttons` | bit 0 Throw, bit 1 Dash, bit 2 Ability1 (the third ability slot), bit 3 Ability2, bit 4 Taunt; bits 5..7 zero |
| 32..39 | 4 | `subTick` | 1/256 tick: when within the tick the latest button press happened (read only on a tick with a press edge; 0 = at the tick's start) |

Packing in C: `(u64)moveDir | (u64)moveMag << 8 | (u64)aimDir << 16 | (u64)buttons << 24 |
(u64)subTick << 32`.

A button fires on a press edge only: it must be up on the previous applied tick and down on this
one. A held button never re-fires, so a bot that wants to press again releases for at least one
tick. `self.lastButtons` is what the sim applied last tick. Buttons belong to ability slots, not to
kinds: slot 0 fires on Dash, slot 1 on Throw, slot 2 on Ability1, whatever each slot runs
(`self.abilities[s].button` gives the bit). The same struct is a human's input: bots and humans
have one action space.

### 1.3 Optional import

| Module | Name | Signature | Meaning |
|---|---|---|---|
| `env` | `kfm_log` | `(ptr: i32, len: i32) -> ()` | `len` bytes of UTF-8 text at `ptr` in the module's memory. The host discards it or captures it in the match result; it never affects the match. |

A module must import nothing else. A module importing anything else is refused.

### 1.4 Budget and faults

- Each `kfm_act` runs under a fuel budget counted in wasm instructions (one unit per executed
  opcode plus one per function entry, as the runtime meters it), never wall clock, so results do
  not depend on the machine. Default `fuelPerTick` = 100 000 instructions. Instantiation (the
  `start` function and the constructors) and `kfm_init` each run under `fuelInit` = 10 000 000.
  Caps: a module file of at most 4 MiB; a linear memory whose declared minimum is at most 16 MiB
  (a larger minimum is refused at load; the maximum is clamped to it; `memory.grow` past the cap
  returns −1 and never traps); the runtime's interpreter stack is 64 KiB. All of these are host
  settings (`kfm_arena --fuel`), and a contest publishes the values it runs with.
- Loading validates the file size, the imports, the four exports and their signatures and the
  memory, then runs a probe instance that checks `kfm_abi_version` and `kfm_obs_buffer`, so a bad
  module is refused at load, not mid-match. Instantiation is repeated for every `kfm_init`: a host
  that re-inits a bot (a match restart, `memory_on_respawn = wipe`) gives it a fresh memory.
- A trap, or running out of fuel, costs the bot that tick: the host applies the bot's last applied
  input again ("repeat last input", the server's rule for a late network input) and counts a
  substitution in the match result. A held button in the repeated input does not re-fire. The
  instance continues on the next tick with whatever its memory holds. A fault inside `kfm_init`
  (a trap or more than `fuelInit`) leaves the bot dead: every later tick is a substitution and the
  module is never entered again.
- A return value with bits 40..63 set is a fault of its own (`BadReturn`, counted like a trap: the
  input is substituted). A `buttons` byte with bits 5..7 set is reserved; v1 hosts mask those bits
  off.
- The host records per bot: acts, substitutions, traps, over-budget ticks, bad returns, the fuel
  of the last act, the maximum and the total, the fuel of `kfm_init`, and the bytes logged and
  dropped. `kfm_arena` prints them in its result line.
- Compiled C and Rust keep their stack in linear memory behind a mutable global named
  `__stack_pointer`, and a trap or an over-budget exit skips the function's epilogue, so every
  fault would leak a stack frame. If the module exports that global, the host saves it before
  `kfm_act` and restores it after a fault. Export it (wasm-ld `--export=__stack_pointer`, or
  `--export-all`); the SDKs do by default.
- The runtime is single-threaded; nothing a module does can observe another instance, the clock
  or the host, and two instances of one module never share memory.

### 1.5 Input delay

Bot versus bot, the host applies the answer to the observation of tick T on tick T. With humans in
the match it may delay a bot's inputs by the humans' lead: the answer to the observation of tick T
is then applied on tick T + `match.inputDelayTicks`, and the observation says so, so a bot can
predict ahead the way a human does. The delay is a host setting and never changes during a match
for a given slot.

## 2. Observation v1

### 2.1 Conventions

- Little-endian, fixed layout, 5748 bytes, alignment 4. Every field is a fixed-width integer at its
  natural alignment; every hole is an explicit `pad*` field. The first 8 bytes are `version` (1) and
  `size` (5748): an SDK checks both.
- Every array has the pool's or the content table's full capacity, and each entry has a `valid`
  byte (players: `present` and `alive`). An empty or absent entry is all zero. Array order is the
  sim's slot order (players by slot, bananas and pickups by pool slot), stable while the entity
  lives; a freed pool slot can be reused by a new entity.
- Every `pad*` and `reserved*` field is zero in v1. A `reserved*` field is defined in §3 and filled
  by a later mode without changing the layout.
- The observation describes the world before tick `match.tick` is simulated; the input answering
  it is applied on that tick (plus the delay, §1.5).

Units:

| Unit | Encoding |
|---|---|
| Q16.16 | i32 raw fixed point: value / 65536. Arena units for positions, lengths and radii; arena units per tick for velocities and speeds; a plain ratio for multipliers (restitution, `speedMul`). |
| angle8 | u8 1/256 turn, 0 = +x, counter-clockwise toward +y (the `PlayerInput` encoding; a player's facing is exactly its aim byte). |
| bam | u16 1/65536 turn, same orientation (a banana's heading, which the sim steers at that precision). |
| ticks | 60 per second. Tick numbers are the sim's tick counter. |
| slot | player slot 0..9; `0xFF` = none. |
| tags | u32 bit set: bit 0 Dashing, 1 PerfectDash, 2 HitStun, 3 Shielded, 4 Invulnerable, 5 Eliminated, 6 Slamming, 7 Parrying, 8 FriendlyFire (architecture v0.32: teammates' bananas, dash attacks and slam pushes reach this monkey; a team mode's baseline grants it), 9 Carrying (architecture v0.33: holds a capture flag; no stat change). |

The world: the arena is the box [−halfExtents, +halfExtents] centred on the origin, +x right,
+y up. Everything is full information in v1 (no fog).

Enumerations (byte values):

| Enum | Values |
|---|---|
| relation | 0 None (empty slot), 1 Self, 2 Ally (a teammate in a team mode, since architecture v0.32; a free-for-all has none), 3 Enemy |
| match phase | 0 Lobby, 1 RoundStarting (spawned, frozen), 2 Playing, 3 RoundEnded, 4 MatchEnded |
| mode kind | 0 Rounds, 1 Deathmatch, 2 Survival, 3 TeamRounds, 4 TeamDeathmatch (architecture v0.32), 5 Hill, 6 Capture (architecture v0.33) |
| ability kind | 0 None, 1 Dash, 2 Throw, 3 Blink, 4 Slam, 5 Parry |
| ability phase | 0 Idle, 1 Startup, 2 Active, 3 Recovery |
| banana phase | 0 Outbound, 1 Returning, 2 Grounded |
| pickup kind | 0 Banana, 1 PowerUp, 2 Flag (architecture v0.33: a capture team's flag; `spawner` holds its team, which is also its home zone's index; taken by an enemy of that team that carries no flag yet) |
| hazard kind | 0 Pit (a centre inside is eliminated), 1 Slow (move speed × `speedMul` inside) |

### 2.2 Top level

| Offset | Size | Block | Contents |
|---:|---:|---|---|
| 0 | 16 | `header` | version, size, schema flags |
| 16 | 168 | `match` | mode, clock, roster, scores, host delay, content hash |
| 184 | 44 | `rules` | the content constants a flight helper needs |
| 228 | 244 | `self` | the own monkey |
| 472 | 640 | `players[10]` | every slot, 64 bytes each (self included, relation Self) |
| 1112 | 2816 | `bananas[64]` | every banana pool slot, 44 bytes each |
| 3928 | 512 | `pickups[32]` | every pickup pool slot, 16 bytes each |
| 4440 | 1116 | `arena` | the arena's statics |
| 5556 | 192 | `reservedHeardCalls[16]` | v0.29 calls, 12 bytes each, zero in v1 |
| 5748 | | | end |

In the tables below "Offset" is relative to the block; the absolute offset is the block's plus
the entry index times the entry size for arrays (for example `players[3].posX` is at
472 + 3 × 64 + 16 = 680).

### 2.3 `header` (offset 0, 16 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u32 | `version` | 1 |
| 4 | u32 | `size` | 5748 |
| 8 | u32 | `schemaFlags` | bit 0: fog (visibility fields meaningful), reserved; 0 in v1 |
| 12 | u32 | `reserved0` | 0 |

### 2.4 `match` (offset 16, 168 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u32 | `tick` | the tick this observation is for |
| 4 | u32 | `ticksInPhase` | ticks spent in the current phase (the mode clock while Playing) |
| 8 | u32 | `timeLimitTicks` | the mode's clock (deathmatch: the match, survival: a round), 0 = none |
| 12 | u32 | `contentHash` | XXH32 of the content, as in the replay header |
| 16 | u8 | `modeIndex` | index of the mode in the content |
| 17 | u8 | `modeKind` | mode kind |
| 18 | u8 | `arenaIndex` | index of the arena in the content |
| 19 | u8 | `phase` | match phase |
| 20 | u8 | `round` | current round, 1-based once the match started |
| 21 | u8 | `roundsToWin` | rounds, survival: round wins that take the match |
| 22 | u8 | `scoreToWin` | deathmatch: eliminations that win |
| 23 | u8 | `aliveCount` | monkeys in the arena |
| 24 | u16 | `presentMask` | bit i: slot i is in the roster |
| 26 | u16 | `aliveMask` | bit i: slot i has a monkey in the arena |
| 28 | u8 | `inputDelayTicks` | ticks from this observation to the tick its answer applies on (§1.5) |
| 29 | u8 | `lastRoundWinner` | slot; `0xFF` after a draw or before the first round end |
| 30 | u8 | `pad0` | |
| 31 | u8 | `pad1` | |
| 32 | u16 | `roundStartTicks` | countdown at a round start (everyone spawned and frozen) |
| 34 | u16 | `roundEndTicks` | pause after a round end |
| 36 | u16 | `respawnDelayTicks` | deathmatch: ticks from an elimination to the respawn |
| 38 | u16 | `respawnInvulnTicks` | deathmatch: spawn protection after a respawn |
| 40 | u16 | `reservedTurnTicks` | §3: ticks per turn in a grid mode |
| 42 | u16 | `reservedTicksToLatch` | §3: ticks to the next turn latch |
| 44 | u8[10] | `scores` | per slot: round wins (rounds, survival) or eliminations (deathmatch) |
| 54 | u8[10] | `deaths` | per slot: eliminations suffered this match |
| 64 | u16[10] | `respawnTicks` | per slot: ticks until a dead slot respawns, 0 = none due |
| 84 | u16[10] | `reservedTeamScores` | §3: per team index |
| 104 | u32[16] | `reservedModeData` | §3: the mode's own state |

### 2.5 `rules` (offset 184, 44 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | i32 | `playerRadius` | Q16.16: every monkey's contact radius |
| 4 | i32 | `playerMoveSpeed` | Q16.16 per tick: base walking speed (`self.moveSpeed` is the derived one) |
| 8 | i32 | `bananaRadius` | Q16.16: base banana contact radius (`bananas[i].radius` is the derived one) |
| 12 | i32 | `bananaSpeed` | Q16.16 per tick: base flight speed |
| 16 | i32 | `bananaSpawnOffset` | Q16.16: a thrown banana appears this far ahead of the thrower's centre |
| 20 | i32 | `bananaRestitution` | Q16.16: wall bounce speed scale |
| 24 | i32 | `pickupRadius` | Q16.16: pickup contact radius |
| 28 | u16 | `bananaOutboundTicks` | ticks a banana flies straight before it turns home |
| 30 | u16 | `bananaMaxReturnTicks` | ticks returning before it drops; it drops at once when its owner is gone |
| 32 | u16 | `bananaTurnRateBam` | bam per tick: the most a returning banana turns toward its owner |
| 34 | u16 | `hitStunTicks` | ticks of HitStun after a hit |
| 36 | u8 | `bananaDamage` | base hp per banana hit |
| 37 | u8 | `dashAttackDamage` | hp per dash attack |
| 38 | u8 | `dashAttackMinAmmo` | bananas in hand that arm a dash attack |
| 39 | u8 | `dashAttackConsumesAmmo` | 1: a dash attack spends a banana |
| 40 | u8 | `playerMaxHp` | base max hp |
| 41 | u8 | `playerAmmoMax` | base banana capacity |
| 42 | u8 | `playerAmmoStart` | bananas at a spawn |
| 43 | u8 | `pad0` | |

Flight rule (what a flight helper reproduces; architecture §5.7): each Playing tick a banana's
`ticksInPhase` grows by one; an Outbound banana whose `ticksInPhase` reaches `bananaOutboundTicks`
turns Returning with `ticksInPhase` 0; a Returning banana whose owner is gone, or whose
`ticksInPhase` exceeds `bananaMaxReturnTicks`, is Grounded (it stops and becomes a pickup);
otherwise it turns its heading toward its owner by at most `bananaTurnRateBam` and flies at its
`speed` along it. Then it moves by its velocity, bouncing off walls and the outer box (the
crossed face mirrors the overshoot and scales that velocity component by `bananaRestitution`).

### 2.6 `self` (offset 228, 244 bytes)

All zero except `slot`, `respawnTicks` and the reserved fields while `alive` is 0.

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `slot` | own slot |
| 1 | u8 | `alive` | 1: the own monkey is in the arena |
| 2 | u8 | `hp` | |
| 3 | u8 | `maxHp` | derived (power-ups) |
| 4 | u8 | `ammo` | bananas in hand |
| 5 | u8 | `ammoMax` | derived (power-ups); a perfect catch needs `ammo < ammoMax` |
| 6 | u8 | `lastButtons` | the buttons the sim applied last tick |
| 7 | u8 | `modifierCount` | live entries in `modifiers` |
| 8 | i32 | `posX` | Q16.16 |
| 12 | i32 | `posY` | Q16.16 |
| 16 | i32 | `prevPosX` | Q16.16: position at the start of the last tick |
| 20 | i32 | `prevPosY` | Q16.16 |
| 24 | i32 | `velX` | Q16.16 per tick: last tick's velocity |
| 28 | i32 | `velY` | Q16.16 per tick |
| 32 | i32 | `moveSpeed` | Q16.16 per tick: derived walking speed (power-ups, slow hazards) |
| 36 | i32 | `radius` | Q16.16: contact radius |
| 40 | i32 | `knockbackX` | Q16.16 per tick: a slam's push, added to the velocity for `knockbackTicks` more ticks |
| 44 | i32 | `knockbackY` | Q16.16 per tick |
| 48 | u32 | `tags` | tags |
| 52 | u16 | `hitStunTicks` | ticks of HitStun left (no movement, no abilities) |
| 54 | u16 | `knockbackTicks` | ticks of knockback left |
| 56 | u16 | `invulnTicks` | ticks of spawn protection left |
| 58 | u16 | `respawnTicks` | while dead: ticks until the respawn, 0 = none due |
| 60 | u8 | `facing` | angle8: the aim the sim applied last tick |
| 61 | u8 | `reservedFacingOctant` | §3 |
| 62 | u8 | `reservedRole` | §3 |
| 63 | u8 | `reservedTeam` | §3 |
| 64 | i16 | `reservedCellX` | §3 |
| 66 | i16 | `reservedCellY` | §3 |
| 68 | ability[3] | `abilities` | 48 bytes each: slot 0 (Dash button), 1 (Throw), 2 (Ability1) |
| 212 | modifier[8] | `modifiers` | 4 bytes each, `[0, modifierCount)` live in pickup order |

`abilities[s]` (48 bytes; absolute offset 296 + 48 s). The `def*` fields are the numbers of the
def the slot runs now, so a power-up's ability override shows here.

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `defIndex` | index of the ability def in the content (0 = none) |
| 1 | u8 | `kind` | ability kind |
| 2 | u8 | `phase` | ability phase |
| 3 | u8 | `button` | the `buttons` bit that fires this slot |
| 4 | u16 | `phaseTicks` | ticks spent in the current phase |
| 6 | u16 | `cooldownTicks` | ticks until it can fire again; ready when 0 and Idle |
| 8 | u32 | `activatedTick` | tick of the last activation (0 = never) |
| 12 | u8 | `activatedSubTick` | 1/256 tick: the press stamp of the last activation |
| 13 | u8 | `pad0` | |
| 14 | u8 | `pad1` | |
| 15 | u8 | `pad2` | |
| 16 | u16 | `defCooldownTicks` | ticks from an activation to the next possible one |
| 18 | u16 | `defStartupTicks` | ticks of the Startup phase |
| 20 | u16 | `defActiveTicks` | ticks of the Active phase |
| 22 | u16 | `defRecoveryTicks` | ticks of the Recovery phase |
| 24 | u16 | `defPerfectWindowTicks` | Dash: a banana met within this many ticks of the press is caught |
| 26 | u16 | `pad3` | |
| 28 | i32 | `defSpeed` | Q16.16 per tick: Dash speed; Blink distance per active tick |
| 32 | i32 | `defParam0` | Q16.16: Slam reach |
| 36 | i32 | `defParam1` | Q16.16: Slam push distance |
| 40 | i32 | `dirX` | Q16.16 unit vector: direction captured at the last activation |
| 44 | i32 | `dirY` | Q16.16 |

Timeline of a press on tick T with phase lengths S, A, R: Startup on [T, T+S), Active on
[T+S, T+S+A), Recovery on [T+S+A, T+S+A+R); the next press can fire on T + `defCooldownTicks`. A
dash moves at `defSpeed` along the stick (or the facing when idle) while Active; a throw spawns
its banana along the aim on the Startup → Active transition.

`modifiers[i]` (4 bytes; absolute offset 440 + 4 i):

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `powerUp` | index of the power-up def in the content |
| 1 | u8 | `pad0` | |
| 2 | u16 | `ticksLeft` | ticks it stays live; `0xFFFF` = for good (the mode's baseline power-up, architecture v0.32) |

### 2.7 `players[i]` (offset 472 + 64 i, 64 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `present` | 1: the slot is in the roster |
| 1 | u8 | `alive` | 1: a monkey in the arena (a slot that left keeps it until it dies) |
| 2 | u8 | `reservedVisible` | §3 |
| 3 | u8 | `relation` | relation to self; None for an empty slot |
| 4 | u8 | `reservedTeam` | §3 |
| 5 | u8 | `hp` | |
| 6 | u8 | `maxHp` | |
| 7 | u8 | `ammo` | bananas in hand (with `dashAttackMinAmmo` or more, its dash is an attack) |
| 8 | u8 | `ammoMax` | |
| 9 | u8 | `facing` | angle8 |
| 10 | u8 | `dashing` | 1: mid-dash (tag Dashing) |
| 11 | u8 | `pad0` | |
| 12 | u32 | `tags` | tags |
| 16 | i32 | `posX` | Q16.16 |
| 20 | i32 | `posY` | Q16.16 |
| 24 | i32 | `velX` | Q16.16 per tick |
| 28 | i32 | `velY` | Q16.16 per tick |
| 32 | u16 | `hitStunTicks` | ticks of HitStun left |
| 34 | u16 | `invulnTicks` | ticks of spawn protection left |
| 36 | u16 | `respawnTicks` | while dead: ticks until the respawn, 0 = none due |
| 38 | u16 | `pad1` | |
| 40 | u8[3] | `abilityKind` | ability kind per slot |
| 43 | u8 | `pad2` | |
| 44 | u8[3] | `abilityPhase` | ability phase per slot |
| 47 | u8 | `pad3` | |
| 48 | u16[3] | `abilityCooldownTicks` | ticks per slot until ready |
| 54 | u16 | `pad4` | |
| 56 | u16[3] | `abilityPhaseTicks` | ticks per slot in the current phase |
| 62 | u16 | `pad5` | |

### 2.8 `bananas[i]` (offset 1112 + 44 i, 44 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `valid` | 1: a banana in this pool slot |
| 1 | u8 | `reservedVisible` | §3 |
| 2 | u8 | `phase` | banana phase |
| 3 | u8 | `owner` | slot of the thrower (of the parrier after a parry) |
| 4 | u8 | `ownerRelation` | relation of the owner to self |
| 5 | u8 | `damage` | hp per hit |
| 6 | u8 | `pad0` | |
| 7 | u8 | `pad1` | |
| 8 | u16 | `headingBam` | bam: flight heading |
| 10 | u16 | `ticksAlive` | ticks since the throw (saturates at 65535) |
| 12 | u16 | `ticksInPhase` | ticks in the current phase (saturates) |
| 14 | u16 | `approachTicks` | ticks until the closest approach to self; `0xFFFF` while self is not alive |
| 16 | i32 | `posX` | Q16.16 |
| 20 | i32 | `posY` | Q16.16 |
| 24 | i32 | `velX` | Q16.16 per tick |
| 28 | i32 | `velY` | Q16.16 per tick |
| 32 | i32 | `speed` | Q16.16 per tick: flight speed (base × the thrower's power-up multiplier) |
| 36 | i32 | `radius` | Q16.16: contact radius (base × the thrower's power-up multiplier) |
| 40 | i32 | `approachDistance` | Q16.16: centre distance at the closest approach; 0 while self is not alive |

Closest approach: the integer k in [0, 600] minimising |pos + k·vel − self.pos| (the banana on its
current velocity, self standing still; computed exactly in 64-bit integers, the smaller k on a
tie), and that distance rounded down. It is a cue, not a prediction: a returning banana curves.
A contact happens when the centre distance falls below `self.radius + radius`.

Which bananas hurt: one in flight (Outbound or Returning) whose owner is not self hits on contact,
unless a dash's perfect window is open with `ammo < ammoMax` (a catch), a parry's window is open,
or the monkey is protected. The own Outbound banana flies through its thrower; the own Returning
banana is caught on contact.

### 2.9 `pickups[i]` (offset 3928 + 16 i, 16 bytes)

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `valid` | 1: a pickup in this pool slot |
| 1 | u8 | `kind` | pickup kind |
| 2 | u8 | `powerUp` | PowerUp: index of the power-up def in the content |
| 3 | u8 | `spawner` | PowerUp: index of its spawner in `arena.spawners`; Flag (architecture v0.33): the flag's team |
| 4 | i32 | `posX` | Q16.16 |
| 8 | i32 | `posY` | Q16.16 |
| 12 | i32 | `radius` | Q16.16: contact radius |

### 2.10 `arena` (offset 4440, 1116 bytes)

Constant for a match. Boxes are closed, axis-aligned `[min, max]` (i32 Q16.16 `minX, minY, maxX,
maxY`, 16 bytes); points are i32 Q16.16 `x, y` (8 bytes).

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | point | `halfExtents` | the outer box is [−half, +half] |
| 8 | u8 | `spawnPointCount` | live entries of `spawnPoints` |
| 9 | u8 | `wallCount` | live entries of `walls` |
| 10 | u8 | `hazardCount` | live entries of `hazards` |
| 11 | u8 | `spawnerCount` | live entries of `spawners` |
| 12 | point[10] | `spawnPoints` | |
| 92 | box[32] | `walls` | solid: monkeys slide along them, bananas bounce off |
| 604 | hazard[16] | `hazards` | 24 bytes each, below |
| 988 | spawner[8] | `spawners` | 16 bytes each, below |

`hazards[i]` (24 bytes; absolute 5044 + 24 i): offset 0 box `box`; 16 i32 `speedMul` (Q16.16,
Slow); 20 u8 `kind` (hazard kind); 21..23 `pad0..pad2`. A hazard acts on a monkey whose centre is
inside; it never blocks movement or bananas.

`spawners[i]` (16 bytes; absolute 5428 + 16 i): offset 0 point `pos`; 8 u16 `powerUpSet` (bit j:
power-up j may spawn, 0 = any); 10 u16 `firstTicks` (Playing tick of the first spawn); 12 u16
`intervalTicks` (0 = once); 14 u8 `maxAlive`; 15 `pad0`.

### 2.11 `reservedHeardCalls[i]` (offset 5556 + 12 i, 12 bytes)

All zero in v1 (§3).

| Offset | Type | Field | Meaning |
|---:|---|---|---|
| 0 | u8 | `valid` | 1: a call heard last turn |
| 1 | u8 | `slot` | the caller |
| 2 | u8 | `symbol` | the caller's aim byte |
| 3 | u8 | `pad0` | |
| 4 | u32 | `tick` | when it was heard |
| 8 | i16 | `cellX` | the caller's cell when visible |
| 10 | i16 | `cellY` | |

## 3. Reserved fields

Architecture v0.29 (§5.3, §5.12, §8 "Teams, fog and calls") designs squads, fog and calls. v1
reserves their fields so the first such mode fills them without a version bump. All are zero in
v1 except the ones the last column marks as filled (architecture v0.31 fills `self.reservedRole`
with the loadout index the match plays and `match.reservedModeData` with the mode's state;
architecture v0.32 fills the team fields: `players[i].reservedTeam`, `self.reservedTeam`,
`match.reservedTeamScores` and the Ally relation of `players[i].relation` and
`bananas[i].ownerRelation`), and
`header.schemaFlags` bit 0 stays clear until fog exists.

| Field | Meaning once used |
|---|---|
| `players[i].reservedTeam`, `self.reservedTeam` | the slot's team index (`MatchConfig::teams`); `relation` and `bananas[i].ownerRelation` then also report Ally. Filled since architecture v0.32: in a free-for-all slot i is team i (every other slot an Enemy); in a team mode the teammates share an index below the mode's team count. An empty `players[i]` entry stays all zero |
| `players[i].reservedVisible`, `bananas[i].reservedVisible` | 1 when the entity is seen this turn under the mode's vision; unseen entries are zeroed (counts stay at capacity) |
| `reservedHeardCalls[16]` | the calls heard last turn: caller slot, symbol (its aim byte), tick, the caller's cell when visible |
| `self.reservedCellX`, `self.reservedCellY` | the own cell in a grid mode |
| `self.reservedFacingOctant` | the facing quantised to eight directions in a grid mode |
| `match.reservedTurnTicks`, `match.reservedTicksToLatch` | a grid mode's turn length and the ticks to the next latch |
| `self.reservedRole` | the loadout index the slot plays; filled since architecture v0.31 (the match's `MatchConfig::loadout`; since v0.32 the role's in a team mode with `roles`: the slot's position within its team, round-robin) |
| `match.reservedTeamScores[10]` | a team's score (the sum of its slots' `scores`), by team index; filled since architecture v0.32 (in a free-for-all team i is slot i, so entry i equals `scores[i]`) |
| `match.reservedModeData[16]` | the mode's own state (`MatchRules::modeData`: a hill's clock, a flag's state); filled since architecture v0.31: words 0–7 are `modeData[0..8)`, words 8–15 stay 0. The shipped kinds (rounds, deathmatch, survival) leave all of it 0. Architecture v0.33: hill keeps team t's hold clock in the low (t even) or high (t odd) u16 of word t/2 and the team holding the zone alone plus one (0: nobody) in word 5; capture keeps team t's flag in word t (bits 0–1 state: 0 home, 1 carried, 2 dropped; bits 8–15 the carrier's slot; bits 16–31 ticks since the drop). The arena's zones are not in v1 (a later version adds them); a capture bot finds the flags in `pickups` |
| `header.reserved0` | nothing yet |

A module must not depend on a reserved field being zero beyond v1, and must ignore it while
`version` is 1.

## 4. Versioning

- `kfm_abi_version` and `header.version` are the same number: the `Observation` schema version.
  The host refuses a module whose number differs from its own.
- Any change to the byte layout or to the meaning of a field (a field added, removed, moved,
  resized or re-unitised, a capacity changed, an enumeration value renumbered) is a new version and
  a breaking change for every bot. Filling a reserved field with the meaning §3 gives it is not a
  change of meaning and stays version 1.
- Appending new enumeration values (a new ability or mode kind, a new tag bit) is allowed within a
  version: a bot must treat an unknown value as "something else", never as an error.
- Content (arenas, modes, ability and power-up numbers) is data, not schema: it reaches a bot
  through the observation (`rules`, `abilities[s].def*`, `arena`) and `contentHash`, never through
  the version.
- The four exports and the optional import are fixed for the life of the ABI; helpers (nearest
  banana, flight prediction, "can I dash-catch now") belong in the SDKs and run inside the bot's
  own budget.

## 5. Building a module

The C SDK seed (`tools/bots/sdk-c/kfm_bot.h`, a single freestanding C11 header) and the example
bots (`tools/bots/examples/*.c`, built by `tools/bots/build_example.sh`) show the shape every SDK
follows. The header mirrors every struct above with `_Static_assert`s on the sizes and offsets,
and reproduces the engine's sine table, atan2, Q16.16 arithmetic, Rng and one banana flight tick
bit for bit (`example_bot_test.cpp` checks each against the engine), so a bot can simulate the
deterministic flight exactly; its predictor over several ticks is approximate because it cannot
foresee catches, hits or the owner's moves.

Recommended clang flags (any clang that targets wasm32 and finds `wasm-ld`; the emsdk one does):

```
clang --target=wasm32 -mcpu=mvp -mmutable-globals --no-wasm-opt -O2 -std=c11 -nostdlib
      -Wl,--no-entry -Wl,--export=kfm_abi_version -Wl,--export=kfm_obs_buffer
      -Wl,--export=kfm_init -Wl,--export=kfm_act -Wl,--export=__stack_pointer
      -Wl,--allow-undefined -Wl,-z,stack-size=65536 -Wl,--strip-all -o bot.wasm bot.c
```

- `-mcpu=mvp -mmutable-globals` pins the module to MVP wasm plus the exported mutable
  `__stack_pointer`, the portable baseline every host accepts; the reference host (WAMR) also
  accepts bulk memory, sign extension, non-trapping float conversions, multi-value and
  reference types, never SIMD, threads, exceptions or memory64. `--no-wasm-opt` keeps the clang
  driver from running binaryen after the link, which would add features back and make the
  output depend on what is installed.
- No libc: a struct copy or a large zero-fill may compile to a `memcpy`/`memset` import, and the
  host refuses any import but `env.kfm_log`. Copy field by field, or link a tiny implementation
  of your own.
- `-Wl,--allow-undefined` leaves `kfm_log` as the one import; importing memory
  (`--import-memory`) is refused.

