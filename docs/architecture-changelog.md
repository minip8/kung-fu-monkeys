# Kung Fu Monkeys — Architecture changelog

One entry per version of `docs/architecture.md`, oldest first; a new entry is appended at the
end. An entry records what was decided, why, and how it was verified; the document's body holds
the current design, and its status line the current version. The entries up to v0.71 were the
head of `architecture.md` until v0.72 and are unchanged.

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
v0.21 changes: more abilities (§5.4, §5.5, §5.6, §5.9). Buttons belong to ability slots, not
kinds (slot 0 Dash button, slot 1 Throw, slot 2 the new Ability button); a kind's own tags
(PerfectDash) come from an `AbilityBehavior::kindTags` hook, and the catch rule and the tie-break
read the slot that grants the tag. Scripted bots use the new abilities and three corpus entries
exercise them (§9). A third ability slot (`kAbilitySlots` 3) on
`Button::Ability1`, empty (`none`) in the base kit so the default match plays as before (the
extra `AbilityState` per player changed every world checksum; the corpus was regenerated and
checked equal with slot 3 left out of the visitor). Loadouts list 2 or 3 names; a mode may carry
its own (`ModeDef::abilities`, read through `MatchContent::loadout()`); any kind may sit in any
slot or be granted by an `AbilityOverride` (the same-kind rule is gone) as long as the dash and
throw buttons fire something; `kMaxAbilityDefs` 16 (§5.8, §5.10). New kinds, each a behaviour
file, a def in `content/abilities.toml`, one registry line and a scenario test, reached through
`rounds_*` modes whose loadout puts them on the ability button: Blink (a teleport dash, no tags
of its own so a telegraphed blinker is still hit; `PlayerBlinked`); Slam (a radial impulse:
`Tag::Slamming` on the impact tick widens the slammer's contact radius in `CollisionSystem`,
contacts made only through that reach carry `Contact::reach` and match only rows flagged
`reachA`, the new rows 16 (ground a non-own banana) and 26 (push a monkey through the new
`Player::knockback`, which `MovementSystem` adds to the velocity); `SlamLanded`,
`PlayerSlammed`; the new state fields changed every checksum, verified equal with them left out
of the visitor); Parry (`Tag::Parrying` while its window may be open, row 15 judged to the press
sub-tick reflects a non-own banana back at its thrower as the parrier's own, a mistimed parry
falls through to damage, the perfect catch wins a tie; `PlayerParryStarted`,
`BoomerangParried`).
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
`--powerups all|none|a,b` on `kfm_server`, `kfm_client` and `kfm_replay`. The local lobby gains
a settings panel (`LobbyMenu`, headless in `kfm_client_core`): arena with a preview drawn from the
def, mode with its key numbers, one toggle per power-up, bots, volume and mute, driven by the
mouse, arrows/WASD or a joined pad's d-pad and stick (never by game buttons, so joins and any
future ability button stay free), saved between runs (SDL's preference directory natively,
`localStorage` in the browser; flags and URL parameters win; `--no-settings`). Online, the status
window shows the server's `MatchConfig` read-only with the roster, and the match-over banner the
announced next match. The HUD scoreboard becomes one card per slot (palette colour, label, score,
power-up timers), the same card the lobby draws per slot (§7).
v0.25 changes: M4 landed (§11). Integration decisions: `defaultContent()` is the minimal
headless fixture and the TOML under `content/` is a superset extended as data only (§5.10);
state fields added for a feature are visited unconditionally and the corpus is regenerated
rather than kept checksum-stable through conditional encodings (§5.2, §9); toml++ headers only
from the vcpkg port, Windows links vcpkg statically, autotools for the vcpkg `alsa` port (§10);
clang-format 19.1.7 is the formatter of record (§9).
v0.26 changes: bots are a public contract (§8). A bot is a WebAssembly module behind a small
versioned C ABI, sandboxed and metered in instructions per tick; this repository hosts the seam
(`IAgent`, the `Observation` schema, `WasmAgent` over a wasm runtime, the reference bot, the
`kfm_arena` headless match runner) and a separate bots repository holds the SDKs, example bots
and the contest. Dropped: ONNX Runtime, the nanobind `VecEnv`, every training dependency (§1,
§2, §4, §10). Human-likeness is a host setting, not part of the bot: input delay matching the
human lead in mixed matches, none bot versus bot, difficulty tiers only for the built-in bot
(§8). M5 reframed as the agent platform with a new exit criterion; the ladder moves to M6 (§11).
v0.27 changes: bananas pierce by default (`[boomerang] pierce = true`): a hit no longer grounds
the banana, it flies on through the victim and comes home; the `piercing_banana` power-up is
removed, shifting the later power-up indices (§5.6, §5.7, §5.8, §12).
v0.28 changes: the pierce switch is gone. Every banana flies on through the monkeys it hits;
`BoomerangDef::pierce`, `Boomerang::pierce`, `ProjectileModifier::pierce` and their TOML keys
are removed, and a banana no longer grounds on a hit under any content (§5.2, §5.6, §5.7, §5.8,
§5.10, §12).
v0.29 changes: modes along five axes and the grid space (§2, §5.3, §5.4, §5.6, §5.10, §5.12,
§8, §11, §12, Appendices). A mode varies win condition, roster, baseline rules, objectives and
space, with a seam per axis: `ModeBehavior` gains `onRoundStart` and `onPickupTaken` and a
mode-owned `MatchRules::modeData` scratch block; teams ride in `MatchConfig::teams`, reach the
sim as `MatchContent::relation(a, b)` and the rules table as a per-row relation mask (friendly
fire is row 31 behind `Tag::FriendlyFire`); loadouts leave the mode list for a `[[loadout]]` def
and a `MatchConfig::loadout` axis, and team modes hand them out per position as roles; a mode's
permanent baseline is a power-up it names; arenas gain zones, spawn groups and a tilemap
authoring format; `kMaxModes` rises to 32. `ModeDef::space` picks `continuous` (today's sim, bit
for bit) or `grid`: monkeys commit to one of eight neighbouring cells per turn of `turnTicks` and
slide there, contested cells resolve at the latch, facing follows aim, a throw roots for the
turn; bananas stay continuous (any angle, the same flight, collision and rules), the base grid
dash never catches and one monkey per team carries the catcher's dash as a role; the contact
radius becomes a derived stat so grid content fits the hit disc inside a cell. Bots (§8): an
entrant is a team, one module instance per monkey, fog of war and in-game calls (a `call`
ability whose payload is the aim byte) are computed in the observation builder from mode
content, never in the sim; the `Observation` v1 field list reserves team, visibility, calls and
the turn clock. The roadmap gains M5b.
v0.30 changes: M5 landed (§4, §8, §9, §10, §11, §12). `engine/ai` holds `IAgent`, `Observation`
v1 (5748 bytes, pinned in `docs/bot-abi.md`), its fixed-point builder, `AgentHost` (the
per-slot runner with the input delay and the repeat-last rule), the reference bot (a utility AI
over the observation with easy/medium/hard tiers) and `WasmAgent` over WAMR 2.4.5's classic
interpreter with instruction metering; the numbers are pinned (100 k instructions per tick,
10 M for instantiation and `kfm_init`, 4 MiB modules, 16 MiB memory); bots are named by one
`BotSpec` grammar in every host; `kfm_arena` runs matches and tournaments headlessly; the server
and the client drive their bot slots through `AgentHost`, the server delaying bots by the humans'
lead. The no-float check covers `engine/ai`. CI's lint job pins clang-format 23.1.3 and
clang-tidy 23.1.4, the versions developers run locally (PyPI wheels), so a local pass is a CI
pass; clang-format 23.1.3 is the formatter of record (§9).
v0.31 changes: M5b step 1, mode foundations, as built (§5.3, §5.10, §6.6, §8, §9, §11).
`kMaxModes` is 32. Loadouts are a match axis: `[[loadout]]` in `content/loadouts.toml`
(`LoadoutDef`, `ContentDB::loadouts`, `kMaxLoadouts` 16, `findLoadout`), loadout 0 is the
`[player] abilities` under the reserved name `base` and the sim reads it from
`PlayerDef::abilities` itself; `MatchConfig::loadout` rides in `MatchContent`, the replay header
(format version 4), the join messages (protocol version 4), a tenth optional corpus manifest
column, the lobby panel and saved settings, `--loadout NAME` on every host and `?loadout=`;
`ModeDef::abilities`/`hasAbilities` and the `rounds_blink`/`rounds_slam`/`rounds_parry` modes
are gone (loadouts `blink`, `slam`, `parry`), and a mode's `abilities` key is an error that
says where it went. Observation v1 fills `self.reservedRole` with the loadout index.
`ModeBehavior` has its four hooks: `onRoundStart` runs at the end of the shared round start
(present slots spawned, `RoundStarted` pushed, phase RoundStarting at `ticksInPhase` 0) and
`onPickupTaken` right after rows 50 and 51 applied, with a copy of the despawned pickup; the
shipped kinds use shared no-ops. `MatchRules::modeData` (eight `uint32_t`, `kModeDataWords`)
is visited unconditionally and cleared at `MatchStarted`, which changed every world checksum
(corpus regenerated; with it left out of the visitor the gate passed unchanged) and fills
`match.reservedModeData[0..8)`. `MatchContent::modeBehavior` is a test seam that replaces the
kind's row, so a scenario runs a probe mode without a `ModeKind` of its own.
v0.32 changes: M5b step 2, teams, as built (§5.3, §5.4, §5.6, §5.8, §5.10, §6.6, §7, §8, §11,
§12, Appendix A). `MatchConfig::teams` (a team per slot, slot i = team i in a free-for-all)
rides in `MatchContent` (`relation`, `teamOf`, `teamScore`, `loadoutIndexFor`), the replay
header (format 5) and the join messages (protocol 5, 4 bits a slot); every host derives it from
the mode (`sim::teamsFor`, balanced, later slots to the smallest team). Every rule row carries
`relationB`; the rows that hurt, push or answer a hit (15, 16, 20, 24, 25, 26, 30) apply to an
Enemy and each has an Ally twin (17, 18, 21, 27, 28, 29, 31) that needs `Tag::FriendlyFire` on
the receiving monkey. `ModeDef` gains `teams`, `baseline` (a permanent `ModifierList` entry,
`ticksLeft = kPermanentModifierTicks`, applied in full at the spawn) and `roles`; `ArenaDef`
gains `spawnGroups` and `ContentDB::compatible`/`firstCompatibleArena`; kinds `team_rounds`
and `team_deathmatch`, the `standoff` arena (two groups of five), two corpus entries; the
lobby's read-only Teams row and the team scoreboard; Observation v1 fills its team fields and
the reference bot ignores a teammate's banana. Replay checksums unchanged.
v0.33 changes: M5b step 3, objectives, as built (§5.3, §5.6, §5.9, §5.10, §7, §8, §11, §12).
`ArenaDef` gains `zones` (`kMaxArenaZones` 8) and `cellSize`; `zoneOccupants()`; tilemap arenas
(`cell`, `map`, glyphs `# . ~ : 0-9 a-h`, runs merged into boxes, the statics caps kept);
`ContentDB::compatible` adds hill's zone and capture's zone per team; `kMaxArenas` 8 → 16. Kinds
`hill` (`hillTicks`) and `capture` (`flagReturnTicks`), both on deathmatch's clock and respawns;
`PickupKind::Flag` (the team in the pickup's `spawner` byte), `Tag::Carrying`, rule row 52, the
`HillHolderChanged` event; the arenas `hilltop` and `outposts` (the first tilemaps) and two corpus
entries. Observation v1 names the flag (pickup kind 2), the kinds (5, 6) and Carrying (tag bit
9). No world-state field added; replay checksums unchanged.
v0.34 changes: M5b step 4, the grid space, as built (§5.3, §5.4, §5.5, §5.8, §5.10, §5.12,
§7, §11, §12, Appendix A; `docs/bot-abi.md`). The `SpaceBehavior` table (`kfm/sim/spaces/registry.hpp`,
`src/spaces/continuous.cpp`, `grid.cpp`) behind `InputSystem` (latch) and `MovementSystem`
(move); the continuous row carries the facing line and the movement body unchanged.
`ModeDef` gains `space`, `turnTicks`, `dashCells` and `arena` (a mode's default arena);
`Player::moveTarget`/`moveTicks` and `Stats::radius` enter the world state (every world
checksum changed once, corpus regenerated); the contact radius is a stat (`stat = "radius"`)
read by collision, movement, the blink and the observation; `AbilityDef::rootPhases`
(`root_phases`); a dash's perfect window may be 0 (never perfect) or the whole active phase;
`ContentDB::compatible` requires cells for a grid mode; the cell mapping is public
(`kfm/sim/spaces/grid_cells.hpp`). Content: `grid_rounds` (15-tick turns), `grid_rounds_1t`
(1-tick turns), the `grid`/`catcher` kits and their `_1t` twins, `quick_hands`, the `grid_base`
baselines and the `courtyard` tilemap (index 8); `dojo_powerups`' spawners name their five
power-ups. Two corpus entries (20 replays). The presentation draws the radius stat and no timer
for a baseline; the lobby hides baselines and a mode brings its own arena.
v0.35 changes: M5b step 5, the squad contest, as built, and M5b landed (§5.5, §5.9, §5.10, §7,
§8, §11, §12, Appendix A; `docs/bot-abi.md`). The `call` ability kind (`range`) and
`MonkeyCalled{player, a = symbol, b = range}`; `ModeDef::vision` (`kind all|team|own`,
`peripheral`, `cone`, `focus`, `hearing`, `banana_sight`; a fog kind needs cells); fog as a
projection in `buildObservation` (`kfm/ai/vision.hpp`), and in `AgentHost` the turn window
visibility is ORed over, the calls each slot hears (`observe()`, called by every host after
`step`) and the per-instance lifecycle (an eliminated monkey's agent paused,
`RespawnMemory::Wipe` re-inits it on respawn); Observation v1 fills the visible flags, the
heard calls, the cell, the facing octant, the turn length and the ticks to the latch, layout
unchanged; `kfm_arena --team` with team places and `--memory-on-respawn` (also on
`kfm_server`); a grid- and fog-aware reference bot; the client's call shout, ping and F1 Calls
section; the C SDK names the filled fields. The grid modes see under `own` fog and call once a
turn: their two corpus entries changed checksums (the call slot), every other `.checksums` is
byte-identical, and `scripted_10p_grid_rounds_1t_1800t` (5v5) is new (21 replays).
v0.36 changes: M6 split into M6a (ship on the web) and M6b (the bot ladder), and the ladder
designed (§8 "The ladder", §11, §12). Accounts with per-user tokens, teams as entrants, source
submissions built on the server in a throwaway container from a pinned C++ recipe (C++ is the
only ladder language), the five most recent submissions kept with the active one, a `contest`
mode as the season's row of the contest knobs, ratings per entrant over rotated two-sided
matches reproducible from seed plus modules, replays served with their season's content, and a
Rust CLI released through PyPI. The bots repository's contents are named and its relation to
this repository is a pinned release. No code change.
v0.37 changes: M6a's hosting and services decided (§6.4, §6.5, §10, §11, §12, §13.5). A Rust
lobby service (`services/lobby/`) owns room codes, connection tokens, signaling and the server
registry; servers dial it, so a game VM opens only its ICE UDP range; the transport's signaling
source becomes injectable and the built-in `ws://` server stays for development. One
`kfm_server` process per match under systemd until a `MatchHost` is needed. Providers: Google
Cloud Compute Engine first (credits), Hetzner Cloud after, Cloudflare Pages for the client,
Caddy for TLS, Sentry's free tier for errors; everything deployed as container images and
systemd units under `deploy/` with no provider-specific service, so the switch is a day. No
code change.
v0.38 changes: M6a as built in this repository (§6.4, §6.5, §9, §11, §13.5): the lobby
protocol pinned in `docs/lobby-protocol.md`; the Rust lobby service in `services/lobby/`;
external signaling and the lobby client mode on `WebRtcTransport`, `LobbyLink` and
`kfm_server --lobby`; the client's `--lobby`/`--room`/`--create` flow, the Online rows and
`config.js`; `deploy/` and the tag-triggered release workflow. Corrections to v0.37: the lobby
wiring is an adapter beside `ServerHost`, not inside it; the VMs run Ubuntu with containers,
not Debian; `capacity` is the total of human slots, never net of joined humans; the page's
`.js`/`.wasm` are published under a content-hashed directory because their names never change;
the client page moved to GitHub Pages in v0.38 and back to Cloudflare Pages in v0.39.
The deployment itself (accounts, the first VM, the playtest over a real link) is pending.
v0.39 changes: the client page is on Cloudflare Pages after all (§10, §11, §13.5), reversing
v0.38's GitHub Pages: the domain's DNS is at Cloudflare, so one account holds the domain, the
DNS and the page, the custom domain is one dashboard step, `_headers` controls caching and
there is no bandwidth cap. The release workflow's web job is the v0.37 one again. No code
change.
v0.40 changes: the roadmap after M6a (§11). The first production play showed M6a's criterion met
technically but not pleasantly: a room is a match already running with bots, the server's
default settings, no waiting room, no names, a lobby panel drawn as a debug dialog, and a region
list that only names regions with a free server. A new M6b, "Play with friends", closes that:
the online room with a waiting state, the host's settings over the wire and a host role, the
code and link to share, every region listed with its availability (lobby protocol v1.1),
nicknames and scoreboards, a main menu, bots yielding slots, reconnect from the UI, the lead
control checked at high RTT. The bot ladder, M6b since v0.36, becomes M7 unchanged, and the
optional Steam build M8. No code change.
v0.41 changes: M6b step (1), lobby protocol v1.1 (§13.5, `docs/lobby-protocol.md`). The
`regions` reply's `list` is now every registered region (busy ones too) and a new `free` key
names those with a free server; `/healthz`'s `regions` stays the free list, unchanged, so probes
read the same thing. A v1 client reading only `list` may offer a busy region and gets
`no_server` as before; a v1.1 client treats a missing `free` (a v1 lobby) as every listed
region free. `WebRtcTransport::lobbyRegions()` returns `LobbyRegion {name, free}`; the client's
Create room row cycles over every region and marks a busy one "us (no server)", still
selectable (a server can come free between the query and the create); a saved region the lobby
does not list falls back to its first free region, else its first. No sim change.
v0.42 changes: M6b step (2), the online room (§6.3, §6.5, §6.6, §6.7, §11). Protocol version
6: `RoomSettings` (host → server: arena, mode, loadout, power-ups, bot count), `RoomState`
(server → every peer on every change and after each `Welcome`: phase, host slot, settings, the
human and bot slots, the winner when over, each human slot's nickname) and `Start` (host →
server), all reliable; `Hello` gains a nickname of at most 16 bytes of well-formed UTF-8 without
control characters (C0, DEL, C1), checked by encoder and decoder alike (`net::validNickname`;
`Nickname::from` sanitises: drops controls and malformed bytes, trims and collapses spaces, cuts
at a character boundary). `NetServer` keeps each slot's nickname (a reconnect keeps it, a
released slot forgets it), reports `RoomSettings`/`Start` as host events behind a per-peer bucket
of 20 per second, sends the RoomState the host sets and gains `releaseReservations()`. The room
lives in `apps/server/room.hpp` beside `ServerHost` (`HostConfig::room`): Waiting (the host pumps
but never ticks: no sim step at all, the clock rebased every frame, a leaver's slot freed at once),
Playing (as before; a host Start places the bots, the first N of the server's specs padded with
`ref:hard` in the lowest free slots, at most the free slots, and restarts the match at the next
tick with the room's settings and `sim::teamsFor`), Over on `MatchEnded` (no tick, no
auto-restart; the host's Start goes back to Waiting with the same settings and removes the bots).
The host is a slot: the first human slot, then the longest-present remaining one; a slot reserved
for a reconnect stays a member, so a host who drops mid-match keeps the role. A room whose last
human slot goes returns to Waiting (bots removed, settings kept). Settings are valid when the
arena, mode and loadout exist, `ContentDB::compatible(arena, mode)` holds and bots ≤ max players
− 1; anything else (and anything from a non-host, or outside Waiting) is ignored and the RoomState
re-sent. `NetClient` holds its timeline while the room is not Playing (`ticksDue()` 0, clock
restarted each call, no input sent, pings continue) and, when Playing resumes, waits for the new
match's snapshot and a Pong past it, then loads it and runs to estimated server tick + target lead
as the initial sync does. `kfm_server` runs the room by default in both `--port` and `--lobby`
modes; `--auto-start` (implied by `--exit-after-matches`) keeps the M3 policy; a lobby room
assigned with no peer resets the room to the defaults (`ServerHost::openRoom`). The client sends
`--nick` / `?nick=` / the saved `nick` (else a random monkey name picked once and saved), draws a
functional waiting / match-over screen and names human slots by nickname on the HUD. Retires the
M3 follow-up "a server-chosen match config". No sim change; replay checksums unchanged.
v0.43 changes: M6b step (3), the client's front end (§7, §13.4, §13.5, §11). Dear ImGui stays the
drawing library; the client gets a game font, a palette and screens. The font is Lilita One (SIL
Open Font License 1.1, one weight, 28 KB), checked in as the source asset with its licence under
`assets/fonts/` and compiled into `kfm_client` by `cmake/EmbedFile.cmake` (a byte array, as
`content/` is: the browser has no filesystem and nothing is read at run time); Dear ImGui 1.92+
bakes glyphs per size, so one `ImFont` serves the UI (20 px), button (28), heading (36) and title
(76) sizes, and ImGui's own font stays loaded for the F1 overlay. One palette
(`apps/client/ui_style.hpp`: banana yellow, cream on dark jungle green, leaf green, bark brown;
player and team colours stay `presentation::playerColor`) feeds the ImGui theme (rounded) and
the HUD, whose colour literals moved there. The client opens on a main menu (Play online, Play
local, Settings, Quit natively) unless `--autostart`, `--connect`, `--room` or `--create` (or
their query keys) say otherwise, which keep their old paths; `--screen menu|settings|online|local`
opens another screen (screenshots). The screens are headless models in `apps/client/core`
(`MainMenu`, `SettingsMenu`, `OnlineMenu`, `ButtonMenu` in `menus.hpp` / `online_menu.hpp`, unit
tested) driven like the lobby panel: `MenuInput` from every device (nothing joins on a menu),
Enter / Space / a pad's South or Start selects, Escape / a pad's East or Back goes back, the mouse
clicks the same items. Settings holds the player's own settings (the nickname, a text field
sanitised by `Nickname::from`, volume, sound, bot difficulty), saved with the others. Play online
replaces the local lobby's M6a Online section (moved out of `LobbyMenu`): the region with its
availability, Create room, a room code field and Join, Back; leaving a room keeps its code in the
field, so Join rejoins (this replaces "Start rejoins the room made with `--create`"). Without a
lobby URL Play online is greyed, unless `--connect` names a server, which it then joins. Escape
from the local lobby or any online screen returns to the menu, and the local lobby is always
local play now (Start no longer connects in network mode; devices bound there are the slots a
later Play online requests). `IPlatformServices` gains `copyText`, `canShare` and `share`
(`ShareHooks` injected into `LocalPlatformServices`: SDL's clipboard natively,
`navigator.clipboard.writeText` and `navigator.share` in the browser, sharing offered only when
`navigator.share` exists and `navigator.maxTouchPoints > 0`; `Null` has neither). The waiting
screen shows the room code in title size with Copy link (the page's `?room=CODE` link; natively
the build's `KFM_PAGE_URL` when set, else the code, then labelled Copy code) and Share, the roster
as cards (nicknames, the host's crown, you, the bots that take the lowest open slots at the
start, open slots), the host's settings (`LobbyMenu::setRoomRows()`: only the rows a
`RoomSettings` carries, now on the keyboard and pads too, Enter starts) or everyone else's
read-only view with the arena preview, Start match / "Waiting for <host> to start" and Leave.
Scoreboards are a view model (`core/scoreboard.hpp`, a pure function of `RenderHud`, the labels
and the local / bot masks): rows sorted by score, then deaths, then slot (by team first in a team
mode, places shared on a tie), the score column named by the kind (Rounds, Kills, Points, Flags;
v0.57: Score for the deathmatch kinds, whose points are kills plus catches less suicides),
deaths where the kind counts them, the winner crowned and this client's rows outlined online; it
shows after a round in the round kinds (RoundEnded only: the next round's countdown keeps the
screen to its banner and count, v0.55) under a raised banner, and at every match's end. The match-over screen (the winner in their colour, the
final scoreboard, a button row) is Play again (a new seed) / Back to lobby locally and the host's
Play again / Leave online ("Waiting for <host> to play again" for the others). Nicknames label
every slot online, bots too ("BOT4"); local play keeps P<n> / BOT<n> (several players share one
screen, so no saved nick for P1). `shell.html` takes the palette (background, loading line);
`?nick=` already maps to `--nick`. No sim change; replay checksums unchanged.
v0.44 changes: M6b step (4), bots yielding, reconnect from the UI, the lead control at high RTT
(§6.2, §6.6, §13.5, §11, §12). Bots yield (§6.6): in the room flow a joiner that only bots'
slots could fit is welcomed as a spectator and waits (`ServerConfig::queueForBotSlots`,
`NetServer::grantQueued`); outside Playing the highest bot leaves at once, while Playing a bot
whose monkey is off the field (eliminated, or any bot at a round end) leaves and its slot is
granted a tick later, so the sim sees a leave and a join (a deathmatch forfeits the bot's
score and spawns the joiner after `respawnTicks`, a rounds mode at the next round); a yielded
bot does not return within the match (`RoomSettings.bots` is "bots when there is room",
applied at every Start); the room flow's lobby `capacity` is max players. A spectator given a
slot runs forward to the lead at once (`NetClient`). The lead control at high RTT (§6.2, §12):
measured with `kfm_netsim` at 200, 300 and 500 ms with ±20–30 ms jitter and 2–5 % loss, 4 and 10
clients: no saturation, oscillation or resync, but a ±30 ms link's late tail was substituted
(2.2 slot-ticks/s per client at any RTT); the margin band is now jitter-aware (a lower envelope
of the margin reports moves it up), which cuts that to 0.1–0.3 for about 0.6 tick more lead and
leaves a steady link's band as it was; a 500 ms soak gate joins the two. Reconnect from the UI (§6.6, §13.5): a
session that lost its link offers Reconnect, which re-dials with the same token, through the
lobby as a rejoin: lobby protocol v1.2 adds an optional `token` to `join` (the lobby keeps every
token it issued for a live room, re-admits the same token outside `capacity`, closes a socket
still seated under it with `closed`/`replaced`, refuses an unknown one `bad_token`). No sim
change; replay checksums unchanged.
v0.45 changes: keyboard-and-mouse controls (§5.4, §7). The primary keyboard profile is one
device, keyboard + mouse: WASD moves, the cursor aims (the direction from the local monkey to the
cursor's ground point), the left button throws and the right one dashes beside the keys (J/Space,
K/Left Shift; the ability stays L/E). Mouse buttons are `Key`s on the keyboard device, so they edge,
stamp `subTick` and join like keys. The sampler stays world-free: before each sampled tick the
session sets the cursor's ground point (`screenToGround` through the arena camera) and each mouse
slot's aim origin, its monkey in the world about to be stepped (`NetClient::world()`, the
predicted one, online). The cursor on the monkey keeps the previous aim; mouse aim starts on a
mouse move or click and stops when the cursor leaves the window. A click over a UI window is the
UI's (ImGui `WantCaptureMouse`); releases always reach the game. No sim or wire change; replay
checksums unchanged.
v0.46 changes: the pause menu (§7, §6.8). Escape (or the Start of a pad bound to a slot) over a
running match opens a small centred menu over the dimmed match instead of leaving at once
(`PauseMenu` in `apps/client/core/menus.hpp`, a `ButtonMenu` column, unit tested). Locally it
pauses the match (the clock's pause, so presses are discarded and a held button does not fire
on resume) with Resume, Back to lobby and Main menu. Online a server's match cannot pause for one
player: the menu (Resume, Leave room) opens over the running match, `NetSession` keeps
advancing and, while `setInputSuspended(true)`, every local slot sends an idle input (no stick,
no buttons, its last aim) rather than repeating its last one, so a monkey that was running when
Escape went down stands still; on resume held buttons count as used until released, a held
stick moves again. Leave room is the only way out of a running online match: a single Escape no
longer disconnects. While the menu is open every device navigates it, Escape or a pad's East,
Back or Start resumes, Enter / Space / South picks, the mouse clicks, and no press reaches the
session (releases and sticks do, so nothing stays held); it closes itself when the match ends
or the link drops. Escape keeps its meaning on the waiting room, match-over and status screens,
which have their own buttons; P (pause) and O (single step) stay the developer's keys, without
the menu, and wait while it is open. The browser page never requests fullscreen, so Escape
reaches SDL like any key; were the canvas fullscreen, the browser would take the first Escape to
leave it and the next would open the menu. `--screen paused` opens a local match under the menu
(screenshots). No sim change; replay checksums unchanged.
v0.47 changes: releases update the VMs from the deploy workflow (§13.5). A `vms` job between
the images and the page ships `deploy/vm/` to every VM in `KFM_VMS` over an IAP SSH tunnel
(Workload Identity Federation, no stored key; `deploy/gcp/setup_deploy_identity.sh`) and runs
`update.sh`, which refreshes the units and restarts the pool at once (a running match ends;
`--drain SECONDS` waits for the lobby to report no live room, for by-hand updates); the page
is published only after the servers answer the new build, since a client of another build id
is refused. v0.45 (mouse controls) is the client branch in
flight.
v0.48 changes: the charged throw (§5.5, §5.7, §7). Throw is hold and release: the wind-up
lasts while the button is held and the banana leaves on the release, along the aim of that
tick. The hold scales the banana's speed, and the outbound time is fixed, so the reach scales
with it: from `charge_min` (a tap, ×0.6: 5.8 units) evenly to `charge_max` (×1.5: 14.4 units)
over `charge_ticks` (40) held past the startup, then it stays there (it was 9.6 units for
every throw). Speed rather than outbound time, so the flight rule a bot reproduces is unchanged
and `bananas[i].speed` already carries the charge: Observation v1 is untouched, the throw def's
numbers reach a bot through `defSpeed`, `defParam0` and `defParam1`. The machine gains one
generic hook, `AbilityBehavior::holdsPhase` (the slot's button held keeps the current phase
past its duration; a Throw def with a positive rate holds its Startup), and `phaseTicks`
saturates. Content: `charge_min`, `charge_max` and `charge_ticks` on the base throw and Fast
Hands (20 ticks); the grid throws do not charge (the grid replays keep their checksums). The
scripted generator holds a charging throw with a per-tick chance (`chargeHoldPer256`), the
reference bot holds for the charge the target's distance needs plus 0.4, and the renderer
grows the aim bar and rings the turning point while a throw charges. Also a NetClient fix: a
held client (room not Playing) that receives the restarted match's snapshot keeps the restart
guard until `tryResume()` loads it, so a checksum of the new match is not judged against the
old world (the room flow's late joiner saw one mismatch).
v0.49 changes: the cartoon look, step 1 (§7 "Look"). The art direction is **Jungle Toon**, picked
from a three-way style board (Jungle Toon, Ink Temple, Banana Beach) with the Mango Sunset menu
colours: thick dark-brown ink outlines, two-band cel shading with a cool shade tint, loud greens.
The renderer draws it: every monkey, banana, wall, rim block and power-up gets an ink outline (an
inverted hull, new programs `outline` and `skinned outline`, `tuning.hpp` widths); `lit` goes
from three toon bands to two (`kCelBandEdge`, `kShadeBand` × `ArenaLook::shadeTint`) and gains
surfaces read from the world position: grass for the floor (two-tone blotches, tufts, the faint
unit grid), a river for pits (crest lines that drift), mud with swelling bubble rings, mossy rock
for walls. The arena colours move into one `ArenaLook` (`palette.hpp` `kJungleLook`), which the
client's arena preview shares. Yellow moves from player slot 2 to slot 5 (purple the other way)
so no monkey in a match of up to five wears banana yellow. Presentation only: no sim, content or
wire change, replay checksums unchanged. Next: the character (fur and gi), the Mango Sunset UI,
then a theme per arena.
v0.50 changes: the cartoon look, step 2: the character (§7 "Look"). The generated rig gets
vertex colours (glTF `COLOR_0`: rgb the part's own colour, alpha the player-colour mask) and a
cartoon build: brown fur, a bigger head (radius 0.75 of 3.2) with a pale face, dark eyes, round
ears, pale hands, feet and a tail; the gi and a headband (a torus with two tails) wear the slot's
colour. `SkinnedVertex` grows to 72 bytes with the colour (white, so all player colour, when a
file has none); `lit` draws `mix(rgb, colour, alpha)` and fades a whole body toward grey for
an eliminated monkey, fur included. Bananas get brown tips (a `lit` surface over the mesh's u).
Presentation only; replay checksums unchanged.
v0.51 changes: the cartoon look, step 3: the Mango Sunset UI (§7 "Look", `apps/client/ui_style.hpp`).
Panels, HUD cards and the ImGui theme go from dark jungle green with leaf-green buttons to
bark-brown panels with dark ink borders; big buttons are sand at rest and mango when focused or
hovered, with a 3 px ink border and a 5 px ink drop shadow (drawn behind the finished button on a
split draw list); titles, headings, the room code and status lines are gold; banana yellow is
kept for bananas (ammo pips, the host's crown); the dark jungle green stays behind every screen.
The palette names roles (`kMango`, `kGold`, `kSand`, `kBark`, `kInk`) instead of the old leaf
greens. Client only; replay checksums unchanged.
v0.52 changes: the cartoon look, step 4: arena themes the player chooses (§7 "Look",
`kfm/presentation/theme.hpp`). A theme is an `ArenaLook` (colours plus the surface kind of the
floor, walls, pits and slow patches): Jungle (grass, mossy rock, river, mud), Temple (flagstones,
red lacquer walls under jade roofs, misty chasms, rice paddies) and Beach (rippled sand with
shells, wooden crates, sea, tide pools); `lit` gains the paving, sand, chasm, planks and lacquer
surfaces, and every floor gets the unit grid and wall band. A theme is cosmetic and local: each
client draws with its own player's choice, so two players in one match may see different
themes, and nothing reaches the sim, the content hash or the wire. This reverses the first idea
of a `theme` key in the arena TOML, which would have made the theme part of the content every
peer must match. The choice is Auto (each arena's suggested theme, a table in
`theme.cpp`: dojos and courtyards Temple, island and outposts Beach, the rest and any unknown
arena Jungle) or one theme everywhere; it is saved (`theme=`), set by `--theme` / `?theme=`, and
changed in Settings and the local lobby panel (not the room's, which carries only room
settings). The client clears around the arena with the theme's background; the lobby's arena
preview draws in the theme. Replay checksums unchanged.
v0.53 changes: the charge reads on the monkey, not the floor (§7). The v0.48 ground ring at the
turning point is gone (and `RenderPlayer::throwReach` with it). A new clip, `charge`, is a
wind-up pose scrubbed by `throwCharge` rather than played over time: the throwing arm swings
back and up until the banana is above and behind the head, the torso twists the shoulder back,
the free arm reaches forward. At full charge a procedural strain shakes the throwing arm and
the spine (two incommensurate sines, fading in over 0.15 s), on top of the sampled pose. The
release starts the throw clip `charge × 0.1 s` in, past as much of its own wind-up as the
charge pose already did, and owes it a whole clip. The rig gains `hand_r`, an unweighted leaf
under the forearm, and the held banana is drawn there on skinned bodies (the capsule fallback
keeps its fixed spot). The growing gold aim bar stays.
v0.54 changes: the cartoon look, step 5: the game checked against the style board (§7 "Look",
§6.8). The v0.49-v0.52 look kept the board's colours but not its language: noisy surfaces, square
slabs in a wooden box frame, a lanky monkey, buttons coloured by focus. As built now:
*The arena* is one `ground` program over a single quad reaching `kGroundMargin` past the arena,
drawn first: backdrop, pits, land, a dirt cliff under every land edge that faces the camera, slow
patches, wall shadows and the ground ink, every edge from a signed distance in sim units and
anti-aliased over a pixel (no stacked slabs, no noise). The land (arena minus pits) is cut into
rectangles on the CPU (`ground.hpp`); a corner is rounded only where it is free, touching boxes
join with a fillet capped at `kGroundFillet` so a pit is never drawn smaller than it is; the
fields are baked per arena and theme into an RGBA16F texture. Surfaces are flat colour with
sparse marks (ripple arcs, tufts, bubble rings). The wooden frame is gone: the arena edge is a
rounded ink line and the backdrop (`BackdropKind`: canopy leaves, mountains, sea) dresses the
outside only. Walls are rounded blocks (`makeRoundedBlock`, `wallRadius`, `kOutlineWall` 0.08)
with a soft ground shadow. The unit grid is drawn only when the mode's space is grid (reversing
v0.49's grid on every floor); under paving the flagstones are the cells. `lit` loses its floor
and pit branches.
*The monkey*: the rig is rebuilt on the board's proportions: a round head 0.88 of the contact
radius (about 55% of the height, the top still at `kMonkeyHeightOverRadius`), a squat gi that
stays inside `Stats::radius` (the head carries the size, never the footprint), short arms at the
sides, round ears, a curled tail, a headband ring with a knot and ribbon tails. The face is the
board's front view painted on an open shell just above the head (`FACE_TILT` 16°), not geometry,
so it takes no ink hull. The charge clip ends with the hand out beside the head. The skinned
hull is drawn with `kOutlineSkinnedDepthBias`. About 9.4k triangles a monkey (was 2.7k). A
moving monkey no longer plays the `run` clip: it holds idle and glides, still turning to its
heading (`kMonkeyRunCycle`, off; the clip stays in the rig, and its bob and lean go with it).
*The UI*: a button's colour is its role, not its focus: Lead (mango: Play online, Create room,
Start, Play again, Resume, Reconnect), Second (gold) and Plain (sand); focus is a 2 px lift and a
3 px cream ring, disabled is flat bark-light. Every screen is a bark panel (4 px ink border, 24
px corners, 6 px ink drop; buttons 4 / 16 / 5). Panel and HUD text carries an ink outline; names
on cards and scoreboards are cream with the slot colour on a stripe or dot (four slot colours
fall under 4.5:1 on bark); `kAlarm` is #ff7a5e. A monkey's tag sits `kTagLift` above its head.
*Flicker*, three causes found by diffing consecutive frames: a camera shake was drawn down to
1e-4 units, so for ~0.8 s after every hit each hard edge toggled between two pixels; it is now
drawn only while it moves the arena centre by at least `kShakeMinPixels` (1 px). A paused local
match kept animating (clips, water, the strain tremble): presentation time now follows the match
clock (`MatchSession::presentationSeconds`), standing still while paused and advancing one tick
on a single step. Where the swap interval cannot be set (X11 under WSLg) the native client ran
unsynchronised at 200+ fps: it now paces frames at the display's refresh rate
(`platform/frame_pacer.hpp`, 60 Hz fallback, never under Emscripten; `--no-frame-limit` turns it
off). Presentation, platform and client only: no sim, content or wire change, replay checksums
unchanged. Open: the camera stays at 55° where the board is near top-down; the lobby's arena
preview still draws plain rectangles; `ArenaPieceKind::PitRim` pieces are built but no longer
drawn; ground-program cost on real GPUs is unmeasured (1.3-1.7x slower under SwiftShader).
v0.55 changes: playtest polish (§5.3, §5.4, §5.10, §6.8, §7). The round scoreboard leaves the
screen when the next countdown starts (RoundEnded and the match's end only), and
`round_end_ticks` goes 120 → 180 on every mode (3 s to read a round's result; only the round
kinds enter RoundEnded). Mouse aim is a player setting (Settings > Mouse aim, on by default;
`--mouse-aim on|off`, `?mouse_aim=`, saved `mouse_aim=on|off`): off, the keyboard + mouse profile
aims by its move direction as before v0.45 and the clicks still throw and dash
(`InputSampler::setMouseAimEnabled`). The menu highlight follows the last menu device
(`apps/client/core/menu_highlight.hpp`): after mouse motion or a click only the item under the
pointer is highlighted (a touch only while pressed); after a keyboard or pad menu press the
focused item is, and the first such press from mouse mode only reveals the focus unless the
pointer is on it. The web client's first-load flicker: `preserveDrawingBuffer` on the
desynchronized canvas, and a menu screen's measuring frame is not presented
(`platform::debugUiMeasuring()`, `commitWithoutPresent()`); the first is inferred from Chrome's
documentation and unconfirmed on a real GPU, the second was measured (the first frame of the
page and of each screen showed the arena without its panel). Replay checksums: changed, corpus
regenerated because the pause after a round is now 180 ticks (replays without a round end
changed their content hash only); servers and the page must deploy together (content hash). The
reference-bot strength test's elimination bar went 5x → 3x: a sweep of five seed bases put 5x on
the noise mean (3.4x to 8.5x per base at either pause). The other three changes are client and
platform only.
v0.56 changes: the page's canvas is no longer `desynchronized` (§6.8). The web client still
flickered after v0.55, most in its first seconds, never natively. Frame-by-frame captures in
headless Chromium (the drawing buffer read after every `requestAnimationFrame` callback, startup
and four screen changes, DPR 1 and 1.5) found no wrong frame left by the client: every callback
ends with a finished picture or draws nothing (a held measuring frame). What remained was the
desynchronized hint itself: it lets the display read the canvas while a frame is being drawn,
and every frame starts by clearing it, which Chrome's guidance for the hint names as a flicker
cause that `preserveDrawingBuffer` does not cover; slow first frames (shader and glyph work)
widen that window. The default double-buffered canvas shows only finished frames, as the
native client's swap does; `preserveDrawingBuffer` goes too (only the hint needed it; a
callback that draws nothing still leaves the previous picture up, checked in Chromium). Cost:
up to a frame of display latency in Chromium. Bringing the hint back needs each frame drawn
off screen and copied once into the canvas, and a check on real GPUs (headless Chromium cannot
show the display's reads). `tools/web/check_browser.py` now fails a desynchronized context.
Client page only: no sim, content or wire change, replay checksums unchanged.
v0.57 changes: pit falls, and dry rounds (§5.3, §5.6, §5.7, §5.9, §5.10, §5.12, §7, §8). A pit no longer eliminates a monkey
on the tick its centre crosses the edge: the monkey **falls**, and only once its **whole body**
is over the hole. `CollisionSystem` emits the pit contact when the monkey's contact disc (the
radius stat) lies wholly inside the union of the arena's pit boxes (`discOverPits` over
`discWithinBoxes` in `content/arena.hpp`): the union, because a tilemap pit is several boxes and
a monkey straddling two adjoining ones must go; exact in fixed point, because the disc's bounding
square is cut at every box edge crossing it and each piece the disc overlaps is classified by its
doubled midpoint, so no sampling, no float, the same answer on every host and in the bots. A
body touching the edge from inside counts as over it; one with a sliver on the floor does not.
A consequence: a pit narrower than the body (2 × radius) in its narrow dimension takes nobody.
Shipped arenas: island's shores are 3 to 5 units; outposts' two `~~` pits are 2 × 2 (a monkey of
radius 0.5 falls with its centre in the middle 1 × 1); courtyard's two pits, 2 × 1 against the
north and south walls (exactly a body deep: in the continuous modes a monkey fell there only
pressed against the wall), are widened to 2 × 2 (rows 1 and 12 gain `~~`), so they take a
continuous monkey like outposts' and a grid monkey (radius 0.35) as before; spawns, spawn groups,
the spawners and `ContentDB::compatible` are untouched. Row 60 forbids `Dashing` (and `Falling`, via `kGone`), so a dash
carries a monkey over a pit and how wide a pit it clears is the dash's reach (4.4 units), with no
pit-size check anywhere; a dash that ends over a pit falls on the next tick. The fall is
`Tag::Falling` (bit 10) plus `Player::fallTicks`, set from the new `PlayerDef::fallTicks`
(`[player] fall_ticks`, 36 ticks, 0.6 s, at least 1, counting the tick it went in) with
`PlayerFell{player, entity, a = the pit box holding the centre}`; the monkey's velocity, push,
grid commitment and every ability phase are dropped (a charging throw is not thrown), InputSystem
gives it an idle intent with the aim at its facing, the spaces leave it in place, and `kGone`
keeps every rule row off it (bananas fly over it, nobody dashes into or pushes it, it takes
nothing, it holds no zone; bots see bit 10 in `tags`, an append within Observation v1).
`LifecycleSystem` counts the fall down in every phase and on its last tick eliminates it as
before (hp 0, `Eliminated`, `PlayerEliminated` with killer 0xFF, `onEliminated` with no killer),
then despawns it. The round kinds (rounds, survival, team rounds) end no round while a monkey is
falling (`anyFalling`): a monkey that went into a pit is already out, so the last one standing
while another falls wins only once that fall ends, and if it too is gone by then the round is a
draw. **A dead monkey's bananas drop where they are**, for every death: each of its bananas away
from it (outbound or returning) turns Grounded on the spot, right then, and `LifecycleSystem`
lays it as a floor pickup anyone may take (`BoomerangGrounded`, `PickupSpawned`). For a pit that
is the moment the fall begins (the monkey is doomed; the pit row drops them); for any other
elimination, the tick of the elimination (as `LifecycleSystem` despawns the monkey). Before, a
dead owner's banana flew on (still able to hit) until it turned for home and only then grounded.
**Bananas never disappear** (`rules/bananas.hpp`), the one exception being a monkey falling down
a pit with bananas in its hand (they go down with it), besides a round reset and a banana sinking
into a pit. A monkey killed any other way drops every banana it holds as floor pickups,
scattered around where it died (`dropHeldBananas`: one at the spot; several on a ring of
`kBananaScatterRadius`, 0.75, from east in equal steps, each pulled back to the spot if it would
overlap a wall and clamped into the arena box; `BananaDropped{monkey, pickup}`). A catch past
`ammoMax` (the auto-catch of the owner's returning banana; the perfect catch already declines at
`ammoMax`) keeps the hand full and drops the extra banana at the catch point; walking over a
pickup at full ammo leaves it, as before. Ammo above a lowered `ammoMax` (an ammo power-up running
out) is cut by ModifierSystem as before: the extra bananas it granted go with it (a review round
had them drop; the economy below took that back); an ammo power-up's top-up fills only up to the
`ammoMax` it gives (never past it, never taking any), so it makes no phantom bananas. A dash attack's spent banana (`consumes_ammo`, off in
shipped content) drops at the contact. A full pickup pool no longer swallows a banana: it is
stacked on the nearest banana pickup (`Pickup::count`, taken as many as fit, the rest left
lying); the pool cannot fill without banana pickups (power-up spawners hold at most half of its
32 slots, flags at most 8), so there is always one to stack on. The audit found nothing else that
destroys a banana: a throw with the banana pool full keeps its ammo, a shield or a slam grounds
the banana (a pickup), a parry hands it over, a respawn or round start only hands out
`ammo_start`, and no mode hook touches bananas. **A banana that comes down over a pit drops into the hole**: a
grounded banana whose disc (its contact radius) lies wholly over the pits' union (`discOverPits`,
the monkeys' own test) turns `BoomerangPhase::Sinking` (`BoomerangFell{owner, banana}`) instead of
a pickup, for `[boomerang] pit_sink_ticks` (18), then `LifecycleSystem` despawns it; Sinking
matches no rule row (every Boomerang row names Outbound/Returning; a test pins it) and runs in
every match phase. This holds however a banana grounds (its owner's death, the return clock, a
shield or a slam), so no pickup ever lies where only a fall could reach it; before, a banana that
grounded over the sea became an unreachable pickup. A capture flag dropped where a falling
carrier went in can still lie over the pit: its clock sends it home, as before. Every round and
respawn still hands out `ammo_start` as before. (The second review round sent a faller's
bananas flying back into its pit; that is gone with its `ToPit` phase, `Boomerang::target` and
`pit_speed`.) **A round with no banana left ends** (the softlock found when a monkey that had
parried another's only banana fell and, at the time, took both down): in the
round kinds (rounds, survival, team rounds, which the grid modes are), when no live monkey holds
a banana, no banana entity is out (in flight, returning or grounded; one sinking into a pit counts
as gone; a dropped one is a pickup and counts), no banana pickup lies around and no power-up that tops ammo up (an `ammo_max` grant with
`add` > 0) lies around or can still be laid by a spawner, the round ends as the kind's clock-out
would: rounds and team rounds a draw, survival its timeout winner. Only bananas hurt (a dash attack
needs one; hazards other than pits do nothing; a slam only pushes), so on an arena without pits
nothing can change a dry round and it ends at once; on one with pits a monkey can still walk or
be slammed in, so it ends after `dry_round_ticks` (ModeDef, `[[mode]] dry_round_ticks`, default
180: 3 s) dry ticks in a row, counted in the new `MatchRules::dryTicks`. The grid falls by the same rule: a walk falls once its disc is inside the pit cell (with
radius 0.35 on a 1.0 cell, near the end of the slide; at 1-tick turns, on the step's tick, as
before), a dash's slide is `Dashing` throughout and falls only if it ends on a pit cell. The
reference bot scores a fall like a hit with the sim's own disc test, fetches no banana it could
only reach by falling, ignores a falling enemy and dashes across a pit it can clear when that is
the way to its goal (§8); it treats a Sinking banana as harmless (Observation v1's banana phase
gains 3, an append). On the grid it now sees that a step into a pit cell is taken (the
sim lets a walk in), not refused: modelled as staying, such a step tied with standing still and
the hysteresis could pick it, half the grid bots' deaths on courtyard. The presentation draws
the fall from state (§7 "Pit falls"): a startled hop, then a drop under the floor (whose depth
hides the body) where the monkey stands, shrinking, tipping, spinning and darkening with the
banana in its hand, its shadow shrinking away, its tag and aim bar gone; `PlayerFell` adds a
closing ring and a falling slide whistle; a dropped banana comes down with the existing grounding
look (dust, the banana-ground sound) and becomes the pickup; one over a pit sinks into the hole
from hand height with a small ring and a plop; and the elimination bursts out of the pit at floor
level.
**The banana economy is per mode** (ModeDef, `content/modes.toml`, the defaults keeping the round
kinds as above). `death_bananas = "drop" | "vanish"` (`ModeDef::deathBananas`, default drop):
drop is the rule above; vanish takes every banana a dead monkey owns, held and in flight
(`BananaVanished{owner, banana}` for each one away, despawned where it is), and the respawn hands
out `ammo_start` again. A pit fall always takes the held ones down; the ones away follow the mode
from the moment the fall begins (`releaseBananasOf`). Shipped: drop in rounds, survival,
team_rounds and the grid modes; vanish in deathmatch, team_deathmatch, hill and capture, where
dropped bananas piled up over a long match. The point kinds (`scoresPoints`) take
`catch_score` and `suicide_score` (int8, default 0; the loader refuses them on a round kind,
whose score counts round wins): a perfect dash catch of an **enemy's** banana (its thrower known
and on another team) adds `catch_score` to the catcher's slot, the own banana or an ally's
nothing; an elimination with no killer (a pit fall; also a killer equal to the victim) adds
`suicide_score` to the victim's slot (`reportElimination`, the one path every elimination now
takes to the mode). Deathmatch and team_deathmatch ship catch +1 and suicide −1 (measured with
four hard reference bots, `kfm_arena --rotate --seed 7`, 10 matches: a deathmatch to 10 now
ends in 710 to 1412 ticks against 1902 to 3269 without the economy, most points being catches;
the bots perfect-catch often, so `score_to_win` may want raising). **Scores are
signed**: `MatchRules::scores` is `int8_t` (saturating; `rounds_to_win` and `score_to_win` are
capped at 127), `teamScore` an `int16_t` sum, serialised as the same byte for 0..127 (`visitI8`);
`score_to_win` counts the points, so a catch can win and a suicide can take a slot or a team below
zero. `ScoreChanged` carries the score after as int32 bits in `a` and a `ScoreReason` in `b`
(Objective 0, Catch 1, Suicide 2); a suicide's point plays no coin and no ScorePop. The scoreboard
column for the deathmatch kinds reads "Score" (was "Kills": it is kills plus catches less
suicides), the HUD cards and team totals print signed numbers, and the lobby's summary says
"points" for a mode with a catch or suicide score. `banana_regrow_ticks`
(`ModeDef::bananaRegrowTicks`, default 0 = off; hill and capture ship 480, 8 s): a monkey that owns
no banana (none in its hand, none of its own outbound or returning; catches and parries move
ownership) for that many Playing ticks in a row grows one in its hand (`BananaRegrown`; a twinkle
in the hand, a short flash and a rising pop, §7). The clock is `Player::regrowTicks`, run by
`LifecycleSystem` after the tick's catches and despawns; owning a banana by any route resets it,
it stops while the monkey falls and a death ends it (a respawn starts fresh). A mode that regrows
never runs dry. Hill and capture also ship `respawn_ticks = 300` (5 s, every cause of death), so a
pit is no quicker way to rearm than the regrow; deathmatch and team_deathmatch keep 120 (their
suicide costs a point). An ammo power-up's extra bananas vanish when it runs out, in every mode
(above). Observation v1 (bot-abi.md §3): `ObsMatch::scores` becomes `int8_t` and
`reservedTeamScores` `int16_t` (the same bytes for non-negative scores), and the two pad bytes
at offset 30 become `bananaRegrowTicks` (the mode's number, 0 = none); the reference bot needs no
change (empty-handed it fetches the nearest banana, else keeps its distance, and in a regrow mode
the regrow re-arms it).
Replay checksums: changed, corpus regenerated because `Player::fallTicks` and
`PlayerDef::fallTicks` entered the world state and the content. With both left out of the
visitors the gate passed on the old `.checksums` for every entry but the two in which a monkey
falls (`scripted_4p_island_1800t`, `scripted_10p_grid_rounds_1t_1800t`, whose scripted inputs
react to the longer rounds); every other `.kfmr` differs only in its content hash. The
whole-body edge (a second regeneration within v0.57) moved only those same two entries. The final
one, for the dropped bananas, bananas sinking into pits, the dry-round rule and courtyard's pits,
moved every entry: `MatchRules::dryTicks` entered the world state and `pit_sink_ticks` and
`dry_round_ticks` the content (every `.checksums` and content hash), and every scripted match has
deaths with a banana away, which now drops at once instead of flying on, so every scripted
`.kfmr`'s inputs changed too (the generator steers by the bananas); the two empty replays changed
only their content hash. "Bananas never disappear" (dropped held bananas, catch overflow, the
pickup stack) moved all 19 scripted entries again (`Pickup::count` entered the world state, and
held bananas now lie where monkeys die); the two empty replays have no pickups and kept their
checksums and inputs. The per-mode economy moved every entry's `.checksums`
(`Player::regrowTicks` entered the world state, the four ModeDef fields and hill's and capture's
`respawn_ticks` the content hash); with the new fields left out of the visitors and the old
respawn numbers the gate passed on the old `.checksums` for all 17 round-kind entries, and only
the four respawn-mode entries (`scripted_4p_deathmatch_3600t`, `scripted_4p_team_deathmatch_3600t`,
`scripted_4p_hill_3600t`, `scripted_4p_capture_3600t`) changed their inputs; every other `.kfmr`
differs only in its content hash.

v0.58 changes: M6b landed (§11). The friends playtest, step (5) of the M6b sequencing, has
been played over the production deployment and the exit criterion ("a group of friends plays a
full evening from one link without help") is judged met; the code was complete in v0.44 and the
play polish of v0.45 to v0.57 went in before the verdict. The same play closes M6a's open
playtest item; its second region and Sentry project stay open. Next is M7, the bot ladder, in
the order of §11 "M7 sequencing"; it has not started. No code change.
v0.59 changes: the ladder updates the game inside a season (§8 "The ladder", §11, §12). A
season no longer pins one release: it is a time window whose ranking freezes at a date, and
the service runs every match on the current release, recording the release tag beside the
seed and the two submission ids. The pin moves to the match: a disputed result is re-run with
that tag's `kfm_arena` from the GitHub release; a replay is served with its own release's
content and opened in that release's client, since a sim change can replay differently even
when the content hash did not move (the header carries both hashes already), so the page is
published under a versioned path per tag beside the root that serves the newest; the `contest`
row may change between releases inside a season. Ratings persist across a release (a shared
shock to every entrant, not a reason to reset; Glicko-2 would widen the deviation instead); the
service announces releases to entrants and the CLI's `test` defaults to the current release so
an author sees breakage before the scheduler does. Design only: no code change.
v0.60 changes: M7 step (1) landed (§8 "The ladder", §9, §11, §13.5). The `contest` mode is the
last row of `content/modes.toml`, today `grid_rounds_1t` renamed (a test pins the equality, so a
season's change to the row is a deliberate edit), with a 10-player corpus entry
(`scripted_10p_contest_1800t`). The `kfm_arena --json` result is a normative, append-only
contract in `docs/arena-result.md`, pinned by a test over every object's keys, their order and
their JSON types; the match object gains `content_hash` and `build_hash` (the replay header's
two hashes, 8 hex digits like `checksum`) so a result names the content it ran under; the build
hash is zero until a build stamps one (open: nothing stamps the netcode's build id either). A
`v*` tag is now a GitHub release: the deploy workflow builds the headless Release `kfm_arena`
with the wasm runtime on the three CI OSes (Linux fully static, Windows with the static CRT),
smoke-runs it with reference bots and the echo module, and uploads
`kfm_arena-<tag>-{linux-x86_64,macos-arm64}.tar.gz`, `kfm_arena-<tag>-windows-x86_64.zip` and
`SHA256SUMS`; the web job uploads the assembled page as `kfm_client-site-<tag>.tar.gz` and, since
a Pages deployment is a full snapshot, unpacks every earlier release's site asset under
`/<tag>/` on each deploy, so a release's page keeps its path (v0.59's viewer per release); each
copy carries its own `config.js`, so an old page's Play online is refused by the current servers
and the versioned pages are for replays and local play. The workflow has not yet run on a tag
(the Actions, macOS, Windows and Cloudflare `:version` header behaviour is unverified; the first
release after this lands is the check). Replay checksums: unchanged, corpus regenerated because
the content hash moved (a new mode); one new entry.
v0.61 changes: M7 step (2) landed, the bots repository (§8, §9, §11). `minip8/kung-fu-monkeys-bots`
holds the C header copied byte for byte (its CI diffs the copy against this repository at the
commit its `engine.lock` names; the copy here stays normative with `example_bot_test`), the C++
layer `sdk/cpp/kfm_bot.hpp` (Q16.16 and vector arithmetic over the header's exact math, typed
views of Observation v1, the helpers §8 keeps out of the host ABI: nearest enemy and banana,
the exact flight path and time to impact, threats, the dash-catch direction, the grid cells and
commitments, and a `KFM_BOT(T)` macro that defines the four exports over a `constinit`
instance, because wasm-ld with no C runtime runs global constructors at the start of every
exported call), the two C examples and a C++ starter (one directory per bot), the recipe
`tools/build.sh` (every source of the directory in one clang command: the §5 flags, C as C11,
anything with a C++ file as C++20 with `-fno-exceptions -fno-rtti -fno-threadsafe-statics`; it
checks the module's size, its exports, that the only import is `env.kfm_log` and that memory is
not imported), the build image on `emscripten/emsdk:6.0.13` pinned by digest, the same clang
as `BUILT_WITH.txt`, with `tools/build_in_sandbox.sh` running the §8 docker command verbatim,
`tools/fetch_kfm_arena.sh` (the release asset by host, verified against `SHA256SUMS`) and
`tools/build_kfm_arena.sh` (a shallow clone at the pinned commit, the headless Release build,
25 s locally) for the time before the first release, and a CI that checks the header, builds
every example in the image and with a bare emsdk and requires the two modules byte-identical,
and plays each against the reference bot failing on any trap, over-budget tick, bad return or
dead module. Measured: the sandbox and local builds of every example are byte-identical; the
starter beats `ref:medium` 8 of 10 and `ref:hard` 12 of 20 one on one, peaks at 61 k fuel of
the 100 k budget, and loses 5v5 contests to `ref:hard`. Two findings here: the checked-in
`chaser.wasm` was stale since the pit falls (rebuilt; `example_bot_test` plays the module and
never compared it with its source: open, a CI rebuild needs a wasm32 clang on the runners), and
GCC 14.3 cannot build the engine at `-Werror` (a `-Wnull-dereference` false positive in
libstdc++ reached from the content loader), so the bots repository's CI builds the runner on
Ubuntu 26.04; whether §9 keeps GCC 14 in the toolchain line is open. No engine code change.
v0.62 changes: the Linux toolchain floor is GCC 15 (§9). GCC 14 was never in CI and does not
build the engine at `-Werror` (v0.61), so the claim was nominal; the requirement now matches
what CI verifies: GCC ≥ 15 or Clang ≥ 19. If a GCC 14 build is ever wanted, the one warning is
cheap to silence and a CI leg on Ubuntu 24.04 with `gcc-14` is the way to keep it. No code
change.

v0.63 changes: M7 step (3) designed, the ladder service (§8 "The ladder", §11 "M7 step 3
design", §12, §13.5). The service is one Rust binary (Tokio, axum, sqlx) in the bots repository
with Postgres as the system of record and S3-compatible object storage (Cloudflare R2 first,
behind the S3 API so any store serves) for sources, modules, logs and replays; slow work is a
row in a `jobs` table that workers claim with `FOR UPDATE SKIP LOCKED` (no broker), and a worker
is the same binary with `--worker build|match`. Identity is GitHub **or Google** OAuth (v0.63
adds Google): an account may link both, the key is the provider's subject id, never the email.
Sessions are signed cookies for the portal; API tokens are stored hashed and shown once. The
build worker runs the §8 docker command under gVisor on a VM that holds nothing else; match
workers need no container, `kfm_arena` is the sandbox. Scheduling is Swiss-style rounds (each
active submission against a few opponents near its rating, two-sided through `--rotate`), not
all pairs, and the rating is Glicko-2 (decided: its deviation widens at a release, v0.59).
Deployment follows the lobby: images from GHCR, systemd units, Caddy, Sentry; one VM for the API,
Postgres and the match workers, a second for the build worker. Deferred: Kubernetes, a message
broker, a frontend framework (the portal is server-rendered by the same binary). Design only.

v0.64 changes: M7 step (3) landed in the bots repository (§8, §11, §13.5). The service of the
v0.63 design as built, with the departures: the engine's `kfm_arena` is baked into the service
image at build time, verified against the release's `SHA256SUMS`, with an entrypoint that pins
the engine tag to the baked runner (not fetched on the workers at start: every container runs
the same verified bytes and starts offline; moving engines is a pin bump and a bots release);
validation is its own `validate` job after `build`, so a rejected submission keeps its module and
hash; the object store is `rust-s3` (the AWS SDK needs a newer rustc than the crate pins); the
build worker runs as a container with the docker socket on a VM whose docker runtime defaults to
gVisor, so every sibling build container is sandboxed whatever the worker passes; submission
zips are fully decompressed and checked at upload, never on a worker; the upload answers 202
with the whole submission (`docs/ladder-api.md` v1.1). Measured: 61 then 113 tests over Postgres
and MinIO with the real sandbox image and the released v0.5.1 runner; upload to ready in 2.25 s
through the local stack, the module byte-identical to a local sandbox build. Not yet done: the
VMs, the R2 bucket and the OAuth apps (the runbook in the bots repository's `deploy/README.md`
lists the order). Next is step (4), the scheduler and ratings.

v0.65 changes: M7 step (4) landed in the bots repository (§8, §11, §12). Seasons as time
windows (`kfm-ladder season create`, no overlap, one current; a job past its season's end
records nothing, so the board freezes). The scheduler is a task in the API under an advisory
lock, one round per interval by the database clock; Swiss-style pairing in k passes from the top
of the board (a single greedy walk left the bottom team without a game), never the same pair
twice in a round, a pair with a live job counting toward k but getting no new job, seeds
deterministic from (season, round, submissions) below 2^62. The match job runs one `kfm_arena
--mode contest --team A×5 --team B×5 --matches 2 --rotate` for both sides, stores both replays,
records the engine tag, both hashes, sides and per-slot faults (a fault in a ranked game is
counted on the submission, never a rejection), and updates both ratings in the same transaction
(rows created in team-id order: three workers deadlocked otherwise). Decided: the rating is
Glicko-2 (tau 0.5, 1500/350/0.06), applied per game as a one-game period, Glickman's worked
example pinned by a test; at a release RD' = min(350, sqrt(RD² + 100²)) before a team's first
game under the new tag. Validation now plays the ranked shape, 5v5 (the 1v1 smoke match cost a
third less fuel per tick than a ranked game). Routes: seasons, the ladder, matches with a
presigned replay URL and the viewer URL `https://kfm.minip8.com/<tag>/?replay=…` (step 6).
Measured: 155 tests; a 5v5 pair that ends takes 0.3 s of `kfm_arena` on the released v0.5.1
runner, two capped games 5 s. Next: step (5), the CLI, then (6), the viewer; the deployment
(VMs, R2, the OAuth apps) is still to do.

v0.66 changes: M7 step (5) landed in the bots repository, and four ladder decisions (§8, §11,
§12). The CLI is `kfm`, published on PyPI as **`kfm-cli`** (`kfm` is taken by an unrelated
package; `uv tool install kfm-cli`), one static binary per platform through maturin (the Linux
wheel a musl binary tagged for manylinux and musllinux). `login` is the local-redirect browser
flow (API v1.3: `/auth/cli` confirms a `submit` token named for the machine, a single-use
five-minute code stored hashed and the token sealed against it, exchanged over
`/auth/cli/exchange`); `build` runs the pinned image or `--local` with the recipe reimplemented;
`test` fetches and verifies the release's `kfm_arena` and plays the ranked 5v5 shape; `submit
--wait --activate`, the listings, `replay get|open`. The ladder's public host is
**`bots.kfm.minip8.com`** (it will carry more than the ladder; GCP and unit names unchanged).
Decided: **replays are kept 24 hours** (the open retention item): the API's scheduler, under its
advisory lock, deletes older replay objects in batches and nulls the match's key, the match row
keeping its result, seed, submissions and rating change for ever (a dispute re-runs from seed
plus modules); the presigning routes are rate limited per client IP (60 a minute, behind
Caddy's forwarded address); the default round is 30 minutes (five minutes with 20 teams would
write 17 k replays a day into R2); a public `/privacy` page for Google's consent screen, written
from what the service stores. Measured: 182 service tests and 57 CLI tests, the CLI run end to
end against the real service. Not yet done: the deployment and the PyPI trusted publisher.

v0.67 changes: the ladder is deployed (§11, §13.5). `https://bots.kfm.minip8.com` serves the
portal and API from `kfm-ladder-1` (an `e2-small` in australia-southeast1-b beside the game
server: Caddy with a Let's Encrypt certificate, the API with the scheduler, Postgres in a
container with the nightly dump to the bucket, two match workers) and `kfm-build-1` (an
`e2-medium`, the build worker, Docker's default runtime gVisor's `runsc`, no inbound port);
the bucket is Cloudflare R2 `kfm-bots` behind a token scoped to it (the token cannot list or
create buckets, so every tool must skip the bucket check, which the backup script does);
sign-in through a GitHub OAuth app and a Google OAuth client with `/privacy` as the consent
screen's policy; the first season `2026-11` runs to 2026-12-01. The bots repository's
`v0.1.0` built the images and `v0.1.1` the CLI wheels: `kfm-cli 0.1.1` is on PyPI through a
trusted publisher (`uv tool install kfm-cli` then `kfm seasons` against the live ladder
checked) and the release updated both VMs through a second Workload Identity provider
(`github-bots`) on the engine's pool. Lessons: the maturin action adds its own platform tag
(two is the limit); a Windows step env named `PLATFORM` is lost to MSVC's `Platform` (the
engine's v0.5.0, now `KFM_`-prefixed); CI's concurrency rule cancels an in-progress re-run when
main moves. Against the M7 criterion: a bot submitted through the CLI is built and ranked;
"by an account outside the project" and "replayed in the browser from the ladder's link"
remain, the second being step (6), the client's replay viewer. No code change here.

v0.68 changes: M7 step (6) landed, the client's replay viewer (§7 "Replay viewer", §8 "The
ladder", §9, §11, §12, §13.5). `kfm_client --replay SRC` opens a replay straight into the viewer
(no main-menu entry yet), and the page's generic query-to-flag mapping turns `?replay=<url>`
into the same flag, so the ladder's link `https://kfm.minip8.com/<tag>/?replay=` plus the
URL-encoded presigned R2 URL needed no page change. `ReplaySession` (headless, beside
`MatchSession` and `NetSession` with the same render surface) decodes the whole replay up front
and refuses one whose content hash is not the client's, naming both (the viewer is the client of
the replay's own release, v0.59); the build hash is shown, not checked (every build writes 0).
Controls: play/pause, single step, five speeds from 0.25x to 4x (the fixed-step clock's ceiling
raised from 2x to 4x; local play's overlay slider still stops at 2x), restart and seeking from
keyframes (a `WorldState` copy every 600 ticks, recorded as playback or a seek first steps onto
it, so a seek into a span already played or sought through steps fewer than 600 ticks; reaching
an unvisited span crosses it once, ~45 ms for a whole 10-minute replay at the release bench's
~820 k ticks/s). Below 1x the viewer always interpolates, and while still, paused or at the end
it draws the newest state alone. In the browser the file is fetched without blocking (an `EM_JS`
`fetch` polled once a frame, capped at 16 MiB, aborted when the screen is left), which works
because the bucket allows a CORS GET from the page's origin; no export or link option changed.
The screen is a spectator's HUD under a control bar (the headless `ReplayControls` model, keys
and pads, the bar's one- or two-row layout decided by the timeline's width); a failure says why
and offers the main menu, and exits 1 under `--exit-after-frames`. A header claiming more ticks
than its bytes can hold is refused before anything is allocated, and a file over 16 MiB from its
size before it is read. Tests: every corpus replay (22) plays to its end with each tick's
checksum equal to its `.checksums`, natively and under node (the corpus embedded), plus keyframe
seeks, interpolation, speeds, the controls and every refusal; two xvfb ctests that match the
client's output, `client_smoke_replay` and `client_smoke_replay_bad`. Verified: the `dev` build
warning-free, `ctest --preset dev -LE 'soak|network'` 40/40, the headless build 38/38, the wasm
`kfm_client_tests` under node, the four `client_smoke*` tests through a stand-in `xvfb-run` on
WSLg's display (CI's real xvfb not yet run), native screenshots of playback, the end and the
error screen, `check_browser.py` on `?replay=` of a locally served corpus replay (renders,
animates, no console errors), the 404 and unreachable-host paths showing the error screen with
no uncaught exception, and a Playwright script driving the keys, a timeline click and Main menu;
the contest corpus replay ends at tick 1800 on its recorded checksum 0x8607c487. A review pass
then added the keyframes, the interpolation and paused-drawing rules, the allocation caps, the
width-based bar layout, the focus hand-off from a control that turns disabled, hot reload
starting when the viewer leaves for the menu, and output-matching smoke tests; verified again
after it: the `dev` build warning-free, `ctest --preset dev -LE 'soak|network'` 40/40,
`kfm_client_tests "[replay]"` 17 cases and 1856 assertions, the four `client_smoke*` tests
through the stand-in display, the wasm `kfm_client` and `kfm_client_tests` built and the node
run passing, clang-format and clang-tidy clean. Follow-ups (§12): effects and animation in sim
time at 0.25x and 4x, player names, a menu entry or file picker, seeking with a pad's stick, the
fog overlay; the bar's two-row layout is unit tested, not seen on screen. Against the M7
criterion: an account outside the project through the whole path remains, and a tagged engine
release, since a link reaches the page of its match's tag and matches recorded under v0.5.1 link
to a page without the viewer, by design (v0.59). Replay checksums: unchanged (no sim change).

v0.69 changes: the `contest` row becomes king of the hill (§8 "The ladder", §5.10, bot-abi.md
§3). Until now the ladder's row was `grid_rounds_1t` renamed, team elimination; it is now
`kind = "hill"` with that row's squad: the grid space at 1-tick turns, two dash cells, two teams
of five by role (`catcher_1t` and four `grid_1t`), the `grid_base_1t` baseline, `own` fog and
calls. Its numbers count 1-tick turns (a monkey crosses the arena in 24): a point per 60 turns
alone on zone 0, five points win, else the leading team after 3600 (a tied lead is a draw), a
respawn takes 120 with 30 of protection, a dead monkey's bananas vanish and an empty hand grows
one after 120. The arena is a new tilemap, `courtyard_hill` (index 9,
`arenas/09-courtyard_hill.toml`): the courtyard with its middle 2 × 2 block removed and the 4 × 2 floor
between the two 1 × 4 pillars made zone 0, so the hill is walled west and east and open north and
south; everything else, the spawn groups and the Quick Hands spawners are the courtyard's, and
the courtyard itself is unchanged (so are `grid_rounds` and `grid_rounds_1t`). Bots need to know
where the hill is, and Observation v1 had no zones: they now ride in `match.reservedModeData`
words 8–15, which v1 reserved and always left 0 (the sim's `modeData` is 8 words), as the arena's
zones 0 and 1 (closed Q16.16 boxes, all zero for a zone the arena lacks), in every mode and
unfogged. Filling a reserved field keeps version 1 (bot-abi.md §4), so every submitted module
keeps loading and running; a new zone table would have been version 2 and broken them all. Two
zones cover the hill and a two-team capture; a third is not shown. The C SDK seed gains
`KFM_ZONE_WORD` and `KFM_ZONES` (the bots repository's copy must follow: its CI diffs the header
byte for byte). The reference bot is unchanged and still ignores zones, by decision: `ref:hard`
only takes the hill by passing through it, so a hill-aware entrant should beat it. Verified:
`kfm_arena --mode contest` with five `ref:hard` a side, ten rotated matches, all ended by score
(five points in 965 to 1594 ticks, no draws, 16 to 28 kills a side), so respawns on the grid and
the hill's clock work at 1-tick turns. Tests: the loader pins the row (hill rules, the squad
fields equal to `grid_rounds_1t`'s, the arena equal to the courtyard but for its middle) where it
used to pin the rename; an observation test checks the zone words. Replay checksums: changed for
`scripted_10p_contest_1800t` only (now on `courtyard_hill` under the hill rules), corpus
regenerated; every other entry was re-recorded for the new content hash with its checksums
unchanged.

v0.70 changes: the fixes of an architecture audit of the whole tree, none of them a gameplay
change (§5.1, §5.2, §5.3, §5.10, §6.3, §6.4, §6.6, §6.7, §7, §8, §9, §13.4, §13.5; bot-abi.md
§1.4, lobby-protocol.md v1.3, arena-result.md). Bots: the WAMR fuel patch charges bulk-memory and
table ops, and memory and table growth, extra fuel in proportion to their operand (len / 8 per
byte op, one per element, 8192 per grown page), taken before the work, so a metered budget bounds
host work and not only the opcode count; a table is capped at 65536 elements at load
(`maxTableElements`, `LoadErrorKind::TableTooLarge`). Before this one `memory.fill` of 16 MiB cost
one unit. Netcode: a burst of lost authoritative frames is repaired explicitly: the client keeps
the latest first tick of the `AuthInputs` windows it has received and, once that is past the
first frame it lacks, asks for a resync after 150 ms (`gapResyncNs`), as a player or a
spectator; measured before the fix, a player waited about 1.8 s for the rollback-overflow resync
and a spectator froze for good. The loopback emulator gains `setOutage()` (one direction blacks
out without a disconnect, same random draws); the server's human-lead estimate samples only
messages whose acknowledgement moved on (a stale ack had pushed it to 146 ticks after a 3 s
outage); the netsim soak gates require a compared checksum from every client; `JoinMatch` gets a
per-peer bucket of 2 per second, a peer without an accepted `Hello` is dropped after 10 s, and a
client's forward run after a snapshot load or a promotion is capped at 300 ticks whatever the
`Pong` tick says. The build id is real: `KFM_BUILD_ID` (default 0 = an unstamped development
build) is compiled into every host through `kfm::build_id`, release artefacts carry the first 8
hex digits of the tagged commit, and a refused client is told to reload the page; before this
every build sent 0 and "peers must match `build_id`" compared 0 with 0. Lobby protocol v1.3: a
seated client's signaling frame whose `signal` envelope would pass 64 KiB is that client's
protocol error (it used to truncate at the server and close the whole room), and a registered
`LobbyLink` ignores a bad lobby frame instead of dropping every peer; the lobby limits each
client address (open sockets, rooms held, create/join rate, failed joins: refused `limited`),
reading the address from `X-Forwarded-For` only behind a configured `--trusted-proxy`; a room
keeps at most 128 tokens; servers report `players`, so a room no game connection reaches is
released after a minute while used rooms keep the full TTL; the reliable lane drops a peer with
more than 1 MiB unsent. A running match no longer depends on the lobby: a server that loses its
lobby link keeps its connected peers and its room and claims the room back, with its tokens,
when it re-registers, and a client whose data channels are open plays on when its lobby socket
closes (a deploy still ends matches: `update.sh` restarts the servers). The lobby secret reaches
`kfm_server` through the environment in the units. Sim: every `visit()` overload is followed by
a `static_assert` on its struct's aggregate arity (`kfm::fieldCount`,
`kfm/core/field_count.hpp`), so a field missing from the visitor is a compile error, and the
checksum and content-hash mutation tests carry one mutation per field, pinned the same way; the
ability, mode and space registries `static_assert` that every row has every hook; capture's
`modeData` layout is guarded like hill's; `kMaxAbilityDefs` rises from 16 to 32 (the 15 shipped
defs plus `none` had filled it); `kfm_content_tests` checks the scenario tests' hand copies of
shipped defs against the TOML (it found `throw_fast` charging at half its shipped rate, the
power-ups without their visuals and the call def off `grid_call`, all fixed in the copies);
`kfm_corpus_tests` plays every corpus entry and asserts the events its name promises; `step()`
takes an optional `StepStats&` that reports contact-list overflow. Hosts: one `ai::agentSeed`
and one `ai::BotFactory` (`kfm::ai_bots`, which alone links `kfm::ai_wasm`) replace three
copies, each host keeping its own error policy; `AgentHost::step` runs fill, step and observe
for hosts that step the world themselves, and `fill()` checks that the previous step was
observed, so a host can no longer silently drop its bots' heard calls; `kfm_replay stats` reads
its thresholds from the match's content; `MatchSession::content()` is const and `editContent()`
is the explicit edit. Client: `IPlatformServices` carries everything that differs between the
browser and a native build (preference texts, the page URL, the replay download, `canQuit`,
`localFiles`) through one `PlatformHooks` value built in `apps/client/client_platform_native.cpp`
or `client_platform_web.cpp`, and `main.cpp` has no `EM_JS` and no `__EMSCRIPTEN__` branch; the
HUD's view models (`HudView`, the winner's title, the clock's urgent threshold, the match-over
screen's typed actions, the room's bot-slot preview) moved from `hud.cpp` to
`apps/client/core/hud_view.hpp` with unit tests. The renderer's look numbers (about 130: lights, glows, effect
rings and spark bursts as `EffectDisc` / `SparkLook` constants) moved from `renderer.cpp` into
`tuning.hpp`, with 30 probe renders byte-identical before and after. §6.3's message table no longer
lists an LZ4-compressed snapshot or a Chat message (neither was built). Verified on GCC 15 Debug
(dev, 45 tests), the headless Release server build with the soak and real-socket gates (50
tests), Emscripten under node (46 tests), the lobby's Rust suite (47 tests), clang-format 23.1.3
and clang-tidy 23.1.4 over the tree; MSVC and Apple Clang are CI's. Replay checksums: unchanged;
the corpus was re-recorded because `kMaxAbilityDefs` moved the content hash, with every
`.checksums` file byte-identical.

v0.71 changes: an engine release opens its own pin bump in the bots repository (§8 "The
ladder", §11; `deploy.yml`, `deploy/README.md`). The ladder runs an engine fix only once the
bots repository's `engine.lock` moves to the release and that repository is released, and the
bump was five hand edits someone had to remember (v0.70's fuel fix sat behind exactly that).
The bots repository gains `tools/bump_engine.sh <tag>`, the mechanical part: the tag's commit
into `engine.lock`, the SDK header copied byte for byte, `sdk/c/ENGINE_SOURCE.txt` and the
lines of its docs that name the pin, then `tools/check_header.sh`. `deploy.yml`'s new last job
`bots-bump` runs it on a `vX.Y.Z` tag once the release has its `kfm_arena` archives and
`SHA256SUMS`, and pushes `chore/engine-<tag>` there as a pull request. It authenticates as a
GitHub App, `kfm-release-bot` (Contents and Pull requests write on the bots repository only;
`KFM_RELEASE_BOT_APP_ID` and `KFM_RELEASE_BOT_PRIVATE_KEY` here), because a commit pushed with
`GITHUB_TOKEN` starts no workflow and the point is that the bots CI plays the examples with the
new runner before anyone merges. Decided: the automation stops at the pull request. Merging is
a person's (the script reports when the header changed, which is when the C++ layer and the
examples need one), and so is the bots release, which publishes `kfm-cli` to PyPI and restarts
the ladder's VMs and must not happen as a side effect of a game release. Without the variable
the job is skipped; a re-run updates the branch and keeps the pull request. Verified: the
script, run down to v0.6.0 and back, reproduces the v0.7.0 bump's edits to those files byte for
byte from github.com and from a local clone; the job's shell was dry-run locally with the push
and the pull request stubbed. Not yet run: the job itself, which first runs on the next tag.

v0.72 changes: the changelog moves out of `architecture.md` into this file, and `CLAUDE.md` is
cut down to what changes how code is written (issue #72). No design decision changes. The
architecture document was 516 KB and opened with 1100 lines of changelog ahead of §1; it is now
417 KB and opens on the design. The status line drops "proposal": the system it describes is
deployed, so the document is the record of what is built, with the plans in §11 and the open
questions in §12. The convention is otherwise the same: a design change updates the body, bumps
the version in the status line and appends an entry here, in one commit. `CLAUDE.md` loses its
milestone history (the "Project state" narrative and the "Next milestone" bullet, both of which
restated §11 and this file) and the per-version annotations on its commands; the rules, the
commands and the CI lessons stay.

v0.73 changes: a release builds the page beside the images and publishes it right after the
VMs (§9; `deploy.yml`, `deploy/README.md`; issue #65). Since v0.70 a client whose build id
differs from the server's is refused and told to reload, so between the first server's
restart and the new page going up, a player on the old page reloads into the old page. The
`web` job ran entirely after `vms`, build included. Measured on the four releases v0.5.1 to
v0.8.0: the page was published 1 min 39 s to 2 min 44 s after the VMs finished (the issue's
guess of 10 to 20 minutes was wrong: the wasm build is about a minute and a half), on top of
the VM update's own 1 min 25 s or so. `web` is now two jobs: `web-build` (no dependency; the
steps up to the assembled page, packed as the run's `kfm_client-site` artifact) and `web`
(needs `web-build`, `images`, `vms` and `release`: it unpacks the artifact, uploads it
unchanged as the release's site asset, assembles the earlier releases' paths and publishes).
In the v0.8.0 run the steps that remain in `web` took 25 s. Decided: the order stays VMs
then page (the servers must answer the new build before the page serves it); the site is
still assembled after the VMs, not in `web-build`, so its list of earlier releases is as
fresh as before; the VM update itself, the larger part of what is left, is unchanged.
Verified: the workflow parses and its job graph is as described; the pack, unpack and site
assembly steps were run locally for a tag and for a manual run (the page round-trips byte for
byte, the earlier releases' paths are served), and a manual run of the workflow from the
branch passed: `web-build` finished 2 min after the start, beside the images, and `web`
published the preview 36 s after its last dependency finished (the job itself 46 s), with the
root and `/v0.5.0/` to `/v0.8.0/` served. Not yet run: a tag, where `web` also waits for
`vms` and uploads the release's site asset.

v0.74 changes: `kfm_replay` reports contact-list overflow (§5.3 "Scratch capacities";
`apps/replay`, `docs/commands.md`; issue #70). v0.70 made the overflow countable
(`StepStats::contactsDropped`) but only tests read it, so a tuning or content change that
started dropping contacts in real matches would have gone unnoticed outside the corpus.
`bench`, `verify`, `stats` and `record-scripted --stats` now sum it over the run (one helper,
`apps/replay/contact_drops.hpp`, in a small `kfm_replay_core` library so a unit test links
it) and, when any contact was dropped, write one line to stderr after their usual output: the
total, the ticks that dropped any and the worst tick (with its match where a command runs
several). Decided: stderr, and `--quiet` does not hide it, because stdout is the data
(checksums, the stats table, the `OK` line) and a warning a quiet run hides is no warning;
nothing is printed when nothing was dropped, so existing output is unchanged byte for byte;
`verify --fail-on-dropped-contacts` turns the line into an error and the exit status to 1, as
for a checksum mismatch, and is off in the corpus gate, where `kfm_corpus_tests` already
asserts zero. `stats` does not call `step()`: its copy of the pipeline (`statsStep`) reports
its own contact list's count, and its shadow check now also fails when that count differs
from the shadow `step()`'s. No sim change; replay checksums unchanged. Tested end to end
through content alone: `tests/fixtures/content/crowd.toml`, added to a copy of `content/` by
the `replay_contact_drops` ctest, is an arena of sixteen stacked hazards with a baseline that
grows ten invulnerable monkeys until they all touch, which overflows the list within a few
hundred scripted ticks (88 contacts over 11 ticks in the test's match); the test checks the
line, both exit statuses, and that the shipped content prints nothing. It depends on shipped
tuning: if that changes enough that the crowd stops overflowing, the test fails rather than
passing silently.

v0.75 changes: the server sends where the room's bots will go, protocol version 7 (§6.3, §6.6,
§7; issue #68). The waiting screen previewed the bots' slots with a copy of the server's rule
("lowest free slots"), but the client does not know the server's `--max-players`, so with
fewer free slots than bots asked for it showed bots the server would never place: a cap of
three with two humans and two bots asked for showed two bots to come where Start places one.
Decided: the issue's second option. `RoomState` gains the bots' placement, a third slot mask;
while the room waits it is the slots the settings' bots take if the host starts now, and in
Playing and Over the slots bots hold. One function, `server::roomBotSlots(free, count)`,
decides the slots, and both the Waiting `RoomState` and `placeRoomBots` at Start are given its
answer, so the preview and the placement cannot disagree; the client's `roomBotPreview` and
the test that pinned its copy of the rule are deleted, and `roomSlots()` in
`core/hud_view.hpp` draws what the server sent. Carrying the cap instead (the first option)
would have kept two copies of the rule. `kProtocolVersion` goes from 6 to 7 with no
compatibility path: a peer of another build is refused already (the build id, v0.70), so the
bump costs nothing a release does not. `RoomState` is at most 178 B. Not changed: the client
still does not know the cap, so the waiting room shows ten slots and the ones above the cap
read "open, send the link". No sim change; replay checksums unchanged. Tests: the room flow
checks the placement in its four existing cases (the preview before Start, the bots' slots in
Playing and Over, a bot that yields leaving both masks) and a new case at a cap of four with
three bots walks a join, two settings changes, another join and a leave before Start places
exactly the previewed slots; the message round-trips the mask and refuses one that overlaps
the humans. Verified on GCC: headless Debug 45 of 45, the `dev` preset 47 of 47, the
`server-headless` Release preset with the soak and real-socket gates 52 of 52, and through
real binaries: `kfm_server --max-players 3 --bots 3` with two
clients shows "Bots 2" and one bot to come.

v0.76 changes: four follow-ups of v0.75 and of the outage coverage, one commit each (§6.2,
§6.3, §6.6, §7, §13.5; issues #85, #84, #74 and #83). No sim change in any of them; replay
checksums unchanged.

`rollbackOverflows` counts episodes (#85). The client counted a rollback overflow on every
`reconcile()` that found one, and it finds the same one on every frame until the snapshot it
asked for arrives, so the stat measured the wait in frames: 284 for one repair of a 3 s outage
at 300 ms RTT, 502 at 500 ms. It now counts once when the client starts waiting for a snapshot
and not again until one has loaded (any load: a resync, a restart, a room resume, the initial
sync); a second overflow after that counts again. The per-frame figure is not kept: nothing
read it as anything but a count. `resyncRequests` and `gapResyncs` are unchanged, and the two
gates on the stat (`kfm_netsim`'s exit status and the soak test) require zero, which means
what it meant. The outage tests now assert the episode count exactly (one wherever the
backstop repairs, zero below the ring's reach) and that no client asks for or loads another
snapshot in the 3 s after it agrees; with the old counting they fail on 48 == 1.

A spectator catches up at once after a late snapshot (#84). A spectator that loaded a
snapshot older than the frames it already held stepped on from the snapshot's tick and then
sat 15 to 18 ticks behind its newest frame instead of 3, because `ticksDue` jumps a spectator
forward only past `spectatorDelayTicks + kRollbackWindow` and left the rest to
`spectatorRatePpm()`, which `lead.maxDeviationPpm` holds to 2%: about 10 s of watching a
quarter of a second late. Decided: the load itself runs a spectator on to
`spectatorDelayTicks` behind its newest confirmed frame, where the clock would hold it. It
steps only frames it holds (they are contiguous from the snapshot's tick, so it still never
predicts; a debug assertion says so), journals events as any run forward does, and is not a
hard jump: `snapshotsLoaded` counts it. A snapshot that is not older than the stream is
untouched, and so are players. The other option, a lower jump threshold for spectators, would
have left the same lag below the new threshold. Side effect: the initial sync goes through
the same load, so a spectator starts 2 to 3 ticks closer to its delay than before. Measured
in the outage tests at the end of the frame that loads: 15, 16 and 18 ticks before, 3 after;
the test bounds it by the delay plus the frames that can arrive within that frame (twice the
jitter). That bound leaves no room for packet loss just before the load; the lossy cases meet
it at their seeds. Not tested: a spectator promoted to a player right after a late load.

Reconnect survives a lobby restart (#74, its second item). After the lobby restarts, a room is
not live again until its server has redialled and claimed it (1, 2, 4, then every 5 s). A
player who pressed Reconnect in that gap was refused `unknown_code`, which withdrew Reconnect
and left only a join by code, as a new player. Decided: the client retries a rejoin refused
`unknown_code` on its own, 2 s after each refusal and at most 4 times, so for 8 s: the
server's 5 s ceiling plus 3 s for its `register` and the round trips, and five refused joins
in all, half of an address's budget of ten (lobby protocol §7), so a join by code still works
straight after. Only a rejoin (a `join` carrying the token of a room the session was in) and
only `unknown_code`: a first join by code, `bad_token`, `server_gone`, `mismatch` and
`limited` end at once as before. Between attempts the session reads Connecting and the screen
stays on "Reconnecting" with no button, since nothing is asked of the player; after the last
refusal it ends exactly as before ("Room ABCDE not found"). `NetSession` cannot build a
transport, so it schedules the retry (`rejoinRetryDue()`) and the app answers it as it
answers the button. No wire change and no lobby protocol version: `docs/lobby-protocol.md`
§2, §3 and §5 state the client rule. Not covered: a lobby whose host is unreachable rather
than restarted, where a server attempt can hang for its 10 s open timeout and the claim can
land after the window. Not taken: retrying `server_gone`, which can have the same shape. The
issue's first item (an adopted room always counting as used) stays open.

The waiting room knows the server's player cap, protocol version 8 (#83). The client drew all
ten slots and did not know `--max-players`, so the slots above the cap read "open, send the
link" though nobody could join them, and the host's Bots row offered counts the server would
refuse. Decided: `RoomState` carries the cap (u4, the cap − 1, after the placement mask;
valid from 1 to `kMaxPlayers` with no slot of the three masks at or above it), rather than
`Welcome`: the cap never changes, but the room screen then stays a function of the one
message it draws. `kProtocolVersion` goes from 7 to 8, again with no compatibility path
(v0.75 has the reason); `RoomState` is at most 179 B. `roomSlots()` returns `Closed` for a
slot at or above the cap and the waiting room gives those slots no card: the grid ends at the
cap, each remaining card where it was among ten, so at the default cap of 10 the screen is
what it was (compared against the previous build: differences within the renderer's own
run-to-run noise). `LobbyMenu::setPlayerCap()` stops the room's Bots row at the cap − 1, the
server's own rule, which is unchanged; local play never sets it. Not changed: the Bots row
follows the cap, not the free slots, so with two humans at a cap of three it can still read 2
where Start places one; the waiting room shows the one bot that fits (v0.75), and the count
is a setting the server clamps at Start. Verified for the four changes together on GCC:
headless Debug 45 of 45, the `dev` preset 52 of 52 with the real-socket tests
(`net_probe_loopback` failed once and passed 15 of 15 repeats; it does not use anything
changed here), the `server-headless` Release preset 52 of 52 with the soak gates, and through
real binaries a server at `--max-players 3` with two clients showing three cards.

v0.77 changes: CI's legs are made shorter without removing a test (§9; `ci.yml`, the tests'
CMake). A run took about 20 minutes, the length of its slowest leg. Measured on run
42939083286: the sanitizer leg 19.9 min (configure 5.7, build 2.7, tests 11.0), the Windows
and Clang legs 14 to 16 min of which 7.5 to 8.7 were configure, lint 10.2 min of which 9.2 were
`clang-tidy`; the tests of an ordinary leg were 1 to 4 min. Three causes, three changes.
(1) vcpkg built all 16 ports from source on every leg (OpenSSL alone three minutes): run-vcpkg
points vcpkg at the `x-gha` binary cache, which vcpkg has removed and now only warns about.
Each vcpkg leg now writes `VCPKG_BINARY_SOURCES=clear;files,<workspace>/.vcpkg-bincache,readwrite`
to `GITHUB_ENV` after run-vcpkg, restores that directory with `actions/cache/restore` (the
newest cache of its toolchain) and saves it under a key of its contents, only when the
configure built something the restored cache lacked. (2) `ctest` ran one test at a time: every
leg now passes `--parallel 4`. The five tests labelled `network` are `RUN_SERIAL` (they open
real sockets and were already timing-sensitive, issue #88) and the five that start a virtual
display take the `xvfb` resource lock (`xvfb-run -a` races for a display number). Locally
(GCC Debug, 12 cores): 123 s serially, 50 s four at a time, 55 of 55 in five parallel runs.
(3) `clang-tidy` ran as one process over 248 files: the lint job now runs one per core over
batches of six, each batch printing its diagnostics in one piece, and fails when any batch
does (checked: a batch that errors makes the pipeline exit 123). Locally 105 s on 12 cores
with no diagnostics. Not done: sharding `kfm_ai_tests`, the single longest test under the
sanitizers (207 s), which now bounds that leg's test time; a compiler cache; trimming the
matrix on pull requests, which would save runner minutes, not wall time. Measured on the pull
request's own runs (42944132751): the first, with no cache to restore, took 19.0 min and saved
nine caches of 41 to 127 MB (a toolchain's legs each save their own, since two builds of a
package are not byte-identical); the second restored them ("Restored 16 package(s) ... in 559
ms"), every configure fell to under half a minute, and the run took 12.5 min. The sanitizer
leg is still the slowest and now by a wide margin (12.5 min; the next is Windows Release at
8.2, most are 3.5 to 6.5). Its tests fell only from 661 s to 493 s, not to the four minutes
projected: four at a time each test ran about 2.4 times slower (the test times sum to 1571 s),
so the runner gives this workload well under two cores' worth, and `kfm_ai_tests` alone
stretched from 207 s to 400 s. `clang-tidy` likewise went from 549 s to 164 s on a cache hit
rather than to a quarter. Shortening the sanitizer leg further means giving it less to do or
splitting it across two runners, not more parallelism. Also recorded here: v0.73's split of
the page job ran on a tag for the first time with v0.9.0, and the page was published 31 s
after the VM update finished (about two minutes before the split). Decided after those
numbers: the sanitizer leg leaves out the three `kfm_netsim` soak gates (`-LE soak`), which
took 93, 72 and 33 s there four at a time. The `server-headless` Release leg runs them, they
are a gate on desyncs and rollback depth rather than a search for memory errors, and the
`NetServer` and `NetClient` they drive run under the sanitizers in `kfm_netcode_tests`. With
that (run 42948352596) the sanitizer leg took 8.5 min, its tests 318 s, and the run 9.4 min,
the slowest leg now Windows Debug at 8.9; the legs vary by a minute or two between runs of
one commit (`clang-tidy` took 164 s in one and 310 s in the next).

v0.78 changes: an account is held to one team on the ladder, and the house teams are the
exception (§8 "The ladder", "Accounts, tokens and teams"; the bots repository's
`docs/ladder-api.md` v1.5, its PR #20). Until now an account could create any number of teams,
each an entrant with its own rating; seeding the board showed it, since one account made both
house teams through the portal. The rule is on memberships, not on creating a team (a limit on
creation alone leaves the same hole through "add member"): `POST /teams` and `POST
/teams/{id}/members` answer the new code `already_in_team` (409) for an account that is in a
team, a member counting as much as an owner, and an account that left its team may join or
create another. It is judged when a membership is made, under a lock on the user's row (two
requests for one account cannot both pass: a test makes two at once and gets one team; it
fails four runs in five without the lock), and memberships from before stay, so nothing is
removed at deploy. It is enforced in the service rather than by a unique index because the
index could not except the operators. An operator is `users.operator`, set by `kfm-ladder
operator add|remove|list` on the VM as a season is, shown by `GET /me`, and it lifts only this
limit: the flag is the account's, not its teams' (a user an operator adds to a house team is
still held to one), no route is open to an operator that is closed to another account, and
nothing sets it over HTTP. The portal shows the create-team form only to an account that may
use it. Also recorded here, the state this decision came out of: the bots repository released
v0.1.5 on engine v0.9.0 (the game is the same for a bot, since `content/`, `engine/sim`,
`engine/ai` and `apps/arena` are identical between v0.8.0 and v0.9.0), gained `examples/holder`
(the starter that stays on the hill armed or not; 187 wins in 200 against the starter and 189
in 200 against `ref:hard` in the ladder's shape), and season `2026-11`'s board, empty since its
start, was seeded with the two house teams, whose first round was paired within a minute and
recorded engine v0.9.0 on both games. Not done: a route that dissolves a team, which the rule
now makes wanted (the last owner cannot leave, so an account that made a team of one cannot
move to another team); a mark for house teams on the board. Verified in the bots repository:
`cargo fmt --check`, `clippy -D warnings` and 187 tests against `postgres:17`, and the
`operator` command by hand against a local database. Not yet released: the rule takes effect
with the bots release after v0.1.5, and M7's criterion still wants an entrant from outside the
project.

v0.79 changes: the contest's next rules are designed, not built (§11 "Contest design: zones to
capture and keep"; pointers in §8 "The `contest` mode" and §12's grid item). Nothing in the
tree changes with this version but the two documents. The design: a new mode kind `control`
(every zone of the arena has an owner; the team with strictly the most monkeys in a zone it
does not own captures it after `capture_ticks`; an owned zone pays its team's clock whether or
not anyone stands on it, `hill_ticks` of clock a point), its state in `modeData` (four teams,
eight zones), three zones on a new arena, `Observation` version 2 as version 1 plus an appended
`zones` block with the host still serving version 1 modules, a banana at 2 cells a turn with a
per-mode outbound and turn rate and hearing cut to 1, power-ups from about turn 90 as `_1t`
twins, and a starting row (`capture_ticks` 30, `hill_ticks` 60, `score_to_win` 10, respawn 240,
invulnerability 8) with the gates it is tuned against. Chosen by the project's owner on the
measurements below: capture and keep rather than presence, three zones rather than two (so a
new ABI version) or five, and the faster banana together with the power-ups. What was
measured, all with `kfm_arena`, `--mode contest`, five against five, `--rotate --seed 1`, the
bots repository's `starter` and `holder` and the built-in `ref:hard`:
(a) The contest as shipped, 40 games each on this tree: `ref:hard` against itself 18 to 22, a
median of 1479 ticks; `starter` against `ref:hard` 32 to 8 (585 eliminations to 849);
`starter` against itself 18 to 22, a median of 717; `starter` against the `idle` or `chaser`
examples 40 to 0 in 371 ticks, the walkover. No fault, no draw, no game at the clock.
(b) `holder` (the starter that stays on the hill unarmed and walks to its nearest hill cell),
200 games each with engine v0.9.0's released runner: 187 to 13 against `starter` (1705
eliminations to 2564), 189 to 11 against `ref:hard`, and against itself 72 to 80 with 48 draws
and 156 games at the 3600-turn clock, split 50 west to 102 east.
(c) The row's numbers on that mirror, 100 games each (games at the clock, draws, median
ticks; then `holder` against `starter`): as shipped 80, 30, 3660 and 95 to 5; invulnerability
8: 34, 7, 1769 and 92 to 8; respawn 240: 22, 2, 1207 and 63 to 37; respawn 240 with
invulnerability 8: 14, 4, 1031 and 66 to 34; respawn 360 with invulnerability 8: 0, 0, 414 and
53 to 47 (the first won fight decides: a walkover is 371); `hill_ticks` 30: 52, 3, 3660 and 82
to 18. In the `holder` against `starter` runs the east side won 55 to 77 games of 100 in every
one of these, whichever bot stood there.
(d) Majority instead of sole occupancy, a throwaway build of `hill.cpp` (the team with
strictly the most monkeys in the zone holds it), the same 100 games: as shipped otherwise 20,
3, 669 and 98 to 2; invulnerability 8: 9, 7, 668 and 96 to 4; respawn 240 with
invulnerability 8: 0, 0, 658 and 75 to 25, 8.5 points a game. The bots were not written for
it.
(e) Where banana hits come from, a throwaway build of the runner counting `BoomerangThrown`
and `PlayerHit` by the banana's age at the hit, 100 games each as shipped: the share of hits
at an age of 3 to 5 ticks was 4 % (`starter` against itself), 5 % (`holder` against
`starter`), 1 % (`ref:hard` against itself) and 4 % (`starter` against `ref:hard`); at most 1
tick, 53, 61, 92 and 74 %; the rest at 2 ticks or at 6 and more (ricochets and returns). Dash
attacks were 1 to 4 % of all hits, and a game had 44 to 94 banana hits and 21 to 48
eliminations.
(f) Content and host knobs on `starter` against itself, 100 games each, the share at 3 to 5
ticks: hearing 1 or 0 (identical results) 4 %, the banana's `radius_mul` 0.8 and 1.0 (radius
0.28 and 0.35) 6 and 7 %, both 5 %, `--delay 2` and `--delay 4` 7 and 8 % (with 37 and 54
eliminations a game against 23: stale observations walk monkeys into the pits). None is a
fix, and the test is limited by the bots, which throw from 3 cells or nearer.
Not measured: anything of the design itself (the kind, the three-zone arena, the banana at 2
cells a turn, the power-up twins); the numbers in the row are starting points. Not done and
kept open in the section: the side bias's cause (a mirrored-inputs scenario test comes first),
a tie-break at the clock, whether a tie decays the capture progress, whether home zones start
owned. The roles, the reference bot and the ladder service are deliberately unchanged.

v0.80 changes: the contest's control rules are built (§11 "Contest design", now "v0.79
designed; v0.80 built"; §5.3's `control` row and as-built paragraph, §5.7's per-mode flight,
§5.8's speed limit, §5.9's `ZoneCaptured`, §5.10's control content, §7's control in the client
and its effect row, §8's `Observation` version 2, the C SDK and the `contest` row, §9's corpus
expectation, §12's grid items), steps (1) to (3) of that design's order except the release and
the measurements; step (4), in the bots repository, is not started. Built: the `control` kind
(`src/modes/control.cpp`, the loader name, the registry row, `kMaxControlTeams` 4, `capture_ticks`);
the per-mode banana flight (`ModeDef::bananaOutboundTicks` and `bananaTurnRateBam`, TOML
`banana_outbound_ticks` 0..65535 and `banana_turn_rate` in BAM per tick 0..32768, 0 meaning the
`[boomerang]` value, through `sim::bananaOutboundTicksOf`/`bananaTurnRateBamOf` and
`MatchContent::bananaOutboundTicks()`/`bananaTurnRateBam()`, read by ProjectileSystem, the
observation's rules block and the drawn banana arc; `fieldCount<ModeDef>()` 25 to 28) and the
projectile `speed_mul` limit of 8 (`kMaxProjectileSpeedMul`, `radius_mul` still 4); `Observation`
version 2 and the host serving versions 1 and 2, as v0.79 designed it; the arena
`courtyard_zones` (index 10), the power-ups `contest_base` (8) and the twins `big_banana_1t`,
`triplebanana_1t`, `quick_hands_1t`, `shield_1t` (9 to 12; 13 of 16 slots used); the `contest`
row; the client's control cases. Decisions v0.79 left unsaid: a zone pays from the tick it is
captured (the clock gains 1 on the capture tick); a team with no present slot loses its owned
zones' points (its clock still drains, nobody is credited, the remainder carries; a slot of it
present again takes the next point at once, dead or alive, and at the clock a team with no
present slot is not judged, `teamScoreLeader` as in hill); the scripted generator gives a monkey
no target while it stands in any zone its team does not own, and the lowest zone index wins a
distance tie; `capture_ticks` is accepted on every kind, read only by control, default 60; a
mode cannot itself set the banana's outbound or turn rate to 0 (0 is the def's); a version 1
module's header keeps version 2's `schemaFlags` and `reserved0`, so the fog flag still reaches
it, and its buffer needs only 5748 bytes; versions other than 1 and 2 are `AbiMismatch`; the
arena is `courtyard_zones` because `courtyard_control` is over the 15-character name limit, and
`triplebanana_1t` for the same reason; the twins last one and a half times their originals'
count of 15-tick turns (`big_banana_1t` and `quick_hands_1t` 60 turns, `triplebanana_1t` and
`shield_1t` 90); `contest_base` is `grid_base_1t` with `speed_mul` 7.5 (17476 × 7.5 = 131070
raw, 2 cells a turn within the per-tick truncation); the checked-in examples `chaser.wasm` and
`idle.wasm` are rebuilt against the version 2 header (the README requires them to match their
sources) and the old `chaser.wasm` is kept byte for byte as
`tests/fixtures/bots/chaser_v1.wasm`. The numbers chosen: the map is the design's draft
verbatim (zone 0 (−2, −1)..(2, 1), zone 1 (−8, −1)..(−5, 1), zone 2 (5, −1)..(8, 1); spawners
at (−0.5, 3.5) and (0.5, −3.5), first 90, every 240, one alive each); the row `capture_ticks`
30, `hill_ticks` 60, `score_to_win` 10, `time_limit_ticks` 3600, `respawn_ticks` 240,
`respawn_invuln_ticks` 8, `vision.hearing` 1, `banana_outbound_ticks` 3, `banana_turn_rate`
15474 (85° a tick), the roles, calls, regrow 120, vanishing death bananas, space and turns
unchanged. What was measured: (a) the turn rate, by a sweep through `step()` on the shipped
content (every floor cell, 8 aim directions, the owner still or walking 6 turns in 4
directions, 12120 throws per rate), counting the bananas `max_return_ticks` grounds and the
median and p99 turns home: 8192 BAM (45°) 729, 7, 38; 10923 (60°) 267, 6, 25; 12288 (67.5°) 58,
6, 18; 13653 (75°) 36, 5, 16; 14564 (80°) 22, 5, 13; 15474 (85°) 0, 5, 13; 16384 (90°) 211, 5,
12, since at exactly a right angle the banana square-orbits the pillars of an axis-aligned map;
the def's 1200 gives a median of 31 and a p99 of 218. (b) The flight at 85° in the open lane:
5 cells straight out (the 1-cell spawn offset and two turns), bending home on the third turn,
5.17 cells ahead at most, 2.34 cells off the line at most, back in hand 5 turns after the
press; so "3 turns out" is 6 cells of path but a reach of about 5 to 5.5 cells, not 6 straight
(which needs an outbound of 4), and 3 was kept. (c) A smoke run, not the gates: `kfm_arena
--mode contest`, five against five, `--rotate --seed 1`, 40 games each. `ref:hard` against
itself: no game at the clock, no draw, a median of 1229 ticks (720 to 1560), 16.6 points and
33.6 eliminations a game, 20 to 20 by entrant, 16 west to 24 east by side.
`tools/bots/examples/chaser.wasm` against `ref:hard`: no fault, `fuel_max` 3579; `ref:hard`
won 40 to 0, every game 698 to 700 ticks, 10 to 0 with no elimination, because a `ref:hard`
monkey passes through its home zone uncontested and keeps it: the walkover of the new rules.
`ref:hard` ignores zones by design, so none of the design's gates is measured; they need step
(4)'s control-aware `starter` and `holder`. Verified: `tests/scenario/control_scenario.cpp` (13
cases, the last three a team with no present slot, the same at the clock and the scripted
generator's lowest-index tie), `banana_flight_scenario.cpp` (a mode's override, and 0 as the
def's bit for bit), `side_symmetry_scenario.cpp` (the mirrored-inputs verdict below),
`tests/unit/content/contest_test.cpp` and the rewritten contest pin in `loader_test.cpp`, the
version 1 prefix pinned by a hash taken from the original version 1 builder, the fixtures
`peek_v1`/`peek_v2`, `tight_v1`/`tight_v2` and `abi_v0`/`abi_v3`, `chaser_v1.wasm` and the
rebuilt chaser playing one seed checksum for checksum, and the client's HUD, scoreboard, lobby,
tuning and presentation tests. The corpus was re-recorded for the content hash: every existing
replay's checksums are unchanged except `scripted_10p_contest_1800t`, which now plays
`courtyard_zones` with the same seeds and length (3 `ZoneCaptured`, 15 objective points, 11
calls, 4 power-ups taken, the match ending inside 1800 ticks) and whose checksums changed
intentionally; its `kfm_corpus_tests` row now expects a zone captured and a zone's point. The
finished branch, on one Linux machine, the way CI runs it: the lint job's three steps over the
whole tree (clang-format 23.1.3 over 399 files and clang-tidy 23.1.4 over 253 translation units
against the `dev` compile database, no `error` or `warning` line; `check_sim_no_float.sh`
clean); a headless GCC 15 Debug build, 45 of 45 tests outside `soak` and `network`; the `dev`
preset (GCC 15, Debug, the client), 55 of 55; the `release` preset (GCC 15 at -O3 with
`-Werror`, WebRTC on), 55 of 55; `server-headless` (Release), `kfm_server --help` and 52 of 52
with the 3 soak gates and the 4 real-socket tests; `wasm` (Emscripten 6.0.13, node 26), the
browser client built and 48 of 48 under node, the 23 determinism tests among them; `asan`
(address and undefined, no recovery), 52 of 52 outside `soak`; every build without a warning,
and every determinism test green, so no `.checksums` file moved beyond the one above. Not run:
`client_smoke` (the machine has no `xvfb-run`, so ctest does not register it; CI runs it), so
nobody has looked at a control match on screen and the HUD's zone chips were sized blind; the
Windows, macOS and Clang legs; the Rust lobby job (no lobby code changed). The side bias
(§11 "Contest design", §12): the mirrored-inputs test the design put first,
`tests/scenario/side_symmetry_scenario.cpp`, plays the shipped row with each east monkey given
the half turn of its west twin's input and compares the world with its own half turn every
tick. Without banana fights the match is mirrored bit for bit to a 10 to 10 draw with each of
the four power-ups; with banana and dash fights every discrete outcome stays mirrored to the
end, within 64 raw units and 4 BAM (48 hits, 24 eliminations, 16 respawns, 8 perfect catches a
match) or 256 raw units and 64 BAM (with dashes); the old hill row on `courtyard_hill` is
mirrored bit for bit without fights too (to the 3600-tick clock, 0 to 0). Six known
asymmetries are pinned by `[known-asymmetry]` cases: the spawn groups' two offset draws
(mirrored in 6 of seeds 1 to 50), the two spawners' separate power-up draws (alike in 12 of
40), `Fixed::operator*`'s floor nudging every banana toward (−x, −y) by about a raw unit a tick
(a whole unit after about 100 ticks of flight in 2 of 8 seeds on the old hill row, negligible
on the new), a grid cell contest or swap at an equal `subTick` to the lower slot (the west's;
no bot sets `subTick`: 973 of 1585 enemy cell contests in 200 `ref:hard` games went west, 61 %,
about 8 a game), a simultaneous dash exchange to the lower slot (3 in 200 games) and same-tick
respawns in slot order with an Rng tie-break (mirrored among standing monkeys, not into an empty
arena; 318 same-tick pairs in 200 games). The engine leans west where it leans at all, so the
east bias measured on the old hill is the bots' and `engine/ai`'s (fog, line of sight, scan
order), which the test does not cover. On the shipped row, `kfm_arena --mode contest`, five
against five, `--seed 1`, 200 games of `ref:hard` against itself: 105 west to 95 east
(two-sided binomial p = 0.52; a fair coin's 95 % range is 86 to 114), 89 to 111 by entrant with
`--rotate`, no draw, no game at the clock, a median of 1234.5 ticks (712 to 1560). The fixes
(neither moves on a tied cell contest, both blows land in a simultaneous exchange, respawn
points chosen before anyone respawns, one draw for all groups and one per spawner beat,
symmetric rounding in the multiply) each move checksums and are left open, the owner's call.
Not done and kept open in §11: the release; the gate measurements and the tuning of the row
against them; step (4) in the bots repository; whether a tie should decay the capture progress
(it holds); a tie-break at the clock; whether the home zones start owned; the side asymmetries'
fixes (also in §12); `hill_ticks` and `capture_ticks` not in the observation (a bot cannot read
either threshold; hill has had the same gap); the zone-blind walkover. The roles, the reference
bot and the ladder service are deliberately unchanged.

v0.81 changes: the contest's next rules are designed, not built (§11 "Contest design: bananas
as the economy"; pointers in §8 "The `contest` mode", §11's M7 progress and §12's slot-bias
item). Nothing in the tree changes with this version but the two documents. Why: the control
contest built in v0.80 plays and ends, but the owner, on seeing it run, judged it too thin for
a contest of bots: nothing a team does compounds, the monkeys are alike but for the catcher,
the objective is a rectangle to out-number, and the one small map can be hard-coded. No
measurement was made for this version; the two of v0.80 that bear on it are the zone-blind
`chaser` losing 10 to 0 in 700 turns to a monkey that crosses its home zone once, and
`ref:hard`, which ignores zones, ending a game against itself in a median of 1234 turns. The
design: a new mode kind `harvest`, in which the banana is the resource as well as the
ammunition. Bananas come from the opening hands and from trees (pickup spawners that lay a
banana on a beat, with no Rng draw; arena content, bearing in every mode); the hand is the
cargo (`ammo`, three under the kind's baseline); a monkey in its home zone banks one banana
for a point every `bank_ticks`, and a monkey alone in an enemy home takes one back out every
`steal_ticks`; the regrow is off and a respawn is empty-handed (`respawn_ammo`), so nothing
else mints a banana. The beats are global, so the kind keeps no state, `WorldState` does not
change and no existing checksum moves; `modeData` word 0 carries the two beats for bots. The
fight needs no new rule, because the sim as built already prices it in bananas: a throw comes
home in five turns unless it is caught, a perfect dash catch takes the enemy's banana, a death
drops the cargo, a pit takes it, and a dash attack needs a banana in hand, so banking the last
one disarms a monkey. A mode may name a pool of arenas (`arenas`, a bit set), `kfm_arena`
cycles it under `--rotate` and its JSON match object gains an appended `arena` key; the first
pool is four half-turn-symmetric maps (`grove`, drafted in the section, `orchards`, `gorge`,
`burrows`), 15 of the 16 arenas with what ships. No new `Observation` version: mode kind 8 is
appended, a tree is a spawner whose pad byte is filled, the homes are zones 0 and 1, so a
version 1 module plays the kind whole. `contest` becomes the kind when it is built, today's
row kept as `control_1t`: `bank_ticks` 2, `steal_ticks` 6, `score_to_win`
40, the contest's clock, respawn, squad, fog and flight, home trees every 90 turns holding
one, flank trees every 60 holding two, rich trees every 30 holding three. What later balance
patches aim at (targets, not a condition of the row), over 200
games a pairing and pool member with three house bots (`gatherer`, `raider`, `keeper`): at
most 5 % at the clock and 5 % drawn, a median of 1200 to 2700 turns, no house bot beating both
others in more than 65 % of games, 15 to 50 % of banked bananas having changed sides, the team
behind at a third of the game winning at least one in five, each map's sides splitting as a
fair coin does, no fault and `fuel_max` under 80 k. Chosen by the project's owner: the
direction (bananas as the economy, and a pool of maps), that it is written up before
anything is built, and the empty-handed respawn with a home tree per team as its answer.
The rest was left to the write-up by the owner ("whatever is intuitively reasonable"; the
numbers are for balance patches later): stealing from a bank and the lone-defender rule, the
global beats, four maps, the row's numbers, and the tied cell contest's fix. Step (4) of the
control design (control-aware `starter` and `holder`) is dropped: the house bots are written for `harvest`. Considered and kept for later
designs, in order: spending banked bananas (a priced respawn, a loadout chosen at the
respawn), peels as traps, bushes (fog only, no checksum), escalating trees, an opening gate,
homes that grant. Considered and not planned: more than ten monkeys (a third ABI version), a
painted floor, a shared team array. Not measured: anything of the design; every number is a
starting point. Open in the section: whether stealing belongs at all, how well the home tree
answers the empty-handed respawn, a tie-break at the clock, the capacities that bound the maps (8
spawners a map, 16 arenas, 32 pickups). Decided with the kind: the tied cell contest of §12
alternates by the turn's parity (the lower slot on an even turn, the higher on an odd one),
not "neither moves", which would hold two deterministic bots at one banana for ever; it moves
the grid entries' checksums.

v0.82 changes: the contest's harvest rules are built (§11 "Contest design: bananas as the
economy", now "v0.81 designed; v0.82 built"; §5.3's `harvest` row and as-built paragraph, §5.6's
contended pickups, §5.8's trees, the spawners' full pool and the baseline's ammo, §5.9's
`BananaStolen` and `ScoreReason::Stolen`, §5.10's harvest content, §5.12's tied cell contest,
§7's harvest in the client and its effect row, §8's observation, reference bot, SDK, `gatherer`,
`kfm_arena` pool and `contest` row, §9's corpus expectations, §11's M7 progress, §12's
slot-bias and objective items, the glossary), steps (1) to (3) of that design's order except
the release; step (4), in the bots repository, is not started. Built: trees
(`PickupSpawner::tree`, the arena spawner key `tree = true`, `sim::hasTree`); the `harvest` kind
(`ModeKind::Harvest` 8, `src/modes/harvest.cpp`, `sim::homeZoneOf`, `kNoZone`); the `ModeDef`
fields `respawnAmmo`, `bankTicks`, `stealTicks` and `arenaSet` (`fieldCount<ModeDef>()` 28 to
32, `fieldCount<PickupSpawner>()` 5 to 6); `EventKind::BananaStolen` and `ScoreReason::Stolen`
(3), appended; the arena pool in `firstCompatibleArena`, `kfm_arena` (match k on member
(k / T) mod P) and its JSON's appended `arena` key; the arenas `grove`, `orchards`, `gorge` and
`burrows` (11 to 14, 15 of 16 used), the power-up `harvest_base` (13, 14 of 16), the `contest`
row as harvest on the pool and `control_1t` (mode 10, v0.80's row unchanged); `ObsModeKind`
8 and the spawner's `tree` byte (was `pad0`) within version 2, the SDK's harvest names and
`docs/bot-abi.md` §2.10, §3, §3.3; the `gatherer` example; the client's harvest cases; the
scripted generator's harvest steering and three harvest corpus entries; `kfm_replay stats`'s
objective points and thefts. Decisions v0.81 left unsaid or said differently: a perfect dash
catch with a full hand is declined and the banana hits, as the engine has done since v0.16
(v0.81 said it drops at the catcher's feet; kept because a fully loaded carrier that cannot
catch is a trade the kind wants, and no checksum moves; a scenario test pins it); `grantBaseline`
no longer tops the hand up by a baseline's `ammo_max` grant (a picked-up power-up still does;
no shipped baseline granted one, so no checksum moved); a power-up spawner that meets a full
pickup pool skips its beat instead of ending the pass (the same draws), so a later tree still
stacks its fruit; a tree counts in `bananaLeft` only while it has a beat to come, and its
`max_alive` does not count toward the power-ups' half of the pickup pool; a tree counts the
bananas on its cell, stack counts summed, and lays at its `pos`; the beats are `ticksInPhase %
beat == 0` as `onPlaying` sees it (the first Playing tick is 1), banking before stealing, both
in slot order, "living" being `zoneOccupants`'; one theft per raider a beat, from the
lowest-numbered qualifying home; the robbed slot is the team's highest scorer among all its
slots, present or not (the lowest slot on a tie), and a team at or below zero cannot be robbed;
`respawn_ammo` reaches late joiners and is capped at `ammoMax`; `bank_ticks`, `steal_ticks`,
`respawn_ammo` and `arenas` are accepted on every kind; `firstCompatibleArena` prefers the
mode's `arena`, then the pool's first suitable member, and `resolveMatchNames` with no arena
named gives it to a mode that names an arena or a pool, while a mode that names neither keeps
arena 0 when it suits, else the first that does (its error names zones and trees); the tied
cell contest, which v0.81 decided to alternate by the turn's parity (the turn, as first built,
`ticksInPhase / turn_ticks` at the latch), is drawn from the world Rng instead, because the
alternation locked onto the kind's even beats (measured below): the monkeys tied at an equal
press for a cell or a swap (a tie dropped when both already lose to an earlier press) are put
in one Fisher-Yates order a latch, in slot order, count − 1 draws (one for a pair), an earlier
press still winning without a draw and a latch without a tie drawing nothing; a pickup that
several monkeys could take on one tick goes in an order the world Rng draws (rows 50, 51 and
52 carry `InteractionRule::contend`; a Fisher-Yates shuffle of the contenders, whatever their
times of impact, at the place of the first in the contact order; none with a single
contender; `kfm_replay stats` follows it through a `ContactObserver` instead of its own copy of
the contact sort, which had drifted), found by measurement (below); `contest` keeps index 9
and the control row is appended as `control_1t`; every pool map uses all 8 spawners (6 trees, 2
power-up spawners), each tree's `first_ticks` equal to its interval. The numbers: the four maps
(`grove` 24 × 14, the draft verbatim; `orchards` 28 × 16, no rich tree, four flank trees at
(±2.5, ±6.5), a 2 × 4 centre hedge; `gorge` 26 × 16, a pit band on columns 11 to 14 with two
2 × 4 bridges carrying the rich trees; `burrows` 20 × 12, a ring, a narrowed middle lane, trees
at the ends of dead-end tunnels; themes jungle, jungle, beach, temple); home trees every 90
holding one, flank every 60 holding two, rich every 30 holding three; the power-up spawners
drawing `big_banana_1t`, `quick_hands_1t`, `shield_1t`, first at 90, every 240, one alive; the
row `bank_ticks` 2, `steal_ticks` 6, `score_to_win` 40, `time_limit_ticks` 3600,
`respawn_ticks` 240, `respawn_invuln_ticks` 8, `respawn_ammo` 0, regrow 0, drop, `catch_score`
0, the squad, fog and flight unchanged. The scripted generator's grid steering: a breadth-first
search over the cells by the grid's rules (walls and pit cells closed, no corner cutting, the
lowest octant on a tie, a 62 × 62 cap), roles by rank among a team's present slots (the second
raids, the third defends, the rest gather), about 30 µs a call in Debug, a corpus regeneration
2.7 to 3.9 s. What was measured: (a) the east bias. `ref:hard` five against five on the harvest
row, 200 rotated games a map, seed 1, Release: west 243 to east 422 (grove 94/82, orchards
22/143, gorge 84/101, burrows 43/96; two-sided p = 4e-12); with the sides exchanged in the maps
team 0 won 404 to 260, so it followed the geometry, not the slots; `vision = all` 312 to 402,
so not the fog; with one team's bots fed the half-turned observation and their inputs turned
back, orchards 61/69 and 69/78, burrows 73/64 and 74/71, so not the sim: the reference bot's
absolute-direction choices (the explore goal floored to the cell east and south of the centre,
a wall on `orchards`, where the west monkeys stood in the east home 1416 monkey-ticks against
135 and the east banked 757 to 287; a zero vector read as east; one call-symbol encoding for
both sides; an east-first hysteresis, sweep, dodge and blink; lowest-pool-slot ties on a
fetch), all fixed. After: 321 to 329 (p = 0.78; grove 96/80, orchards 75/93, gorge 82/84,
burrows 68/72), seed 1001 329 to 314, sides exchanged 321 to 316, `vision = all` 367 to 336, one
against one 131 to 145, `control_1t` 102 to 98; so the east bias measured since the old hill row
was the reference bot's (and presumably the like in the bots repository's house bots, which
were not re-run). Fog also put a centre exactly on a cell edge in the east and south cell only;
an edge centre now lies in both cells (all four on a corner), found by the new
`side_symmetry_test.cpp`, rare in play and not the bias. (b) A smoke run, not the targets:
`kfm_arena --mode contest --arena NAME`, five a side, 20 rotated games a map, seed 1, `gatherer`
against `ref:hard` before the reference-bot fix: grove a median of 971 ticks, 40.1 to 3.1;
orchards 853, 40.1 to 1.8; gorge 1146, 40.1 to 1.2; burrows 794, 40.1 to 5.2; no fault, no game
at the clock, the gatherers winning all 80; `fuel_max` 27 500 to 34 411. The gatherer mirror was
one deterministic game a map (the bot ignores its seed): a 40 to 40 draw on orchards (874
ticks), gorge (788) and burrows (672), and on grove 40 to 12 for side 0 in every game (490
ticks), because both teams' first picks are the same rich tree and the lower slot took the
fruit both teams' monkeys touched when it was laid, which is what led to the drawn pickup order.
Re-run on the finished branch (Release): the gatherers win all 80 against `ref:hard` (medians
1304, 1468, 1285 and 1100 ticks), and the pool end to end (no `--arena`, 16 rotated matches,
seed 1) cycles grove, grove, orchards, orchards, gorge, gorge, burrows, burrows and repeats,
every game ending by score with no fault, the gatherers winning all 16 (medians 1051, 1591,
1414 and 1270 ticks). (c) The tied cell contest. The alternation by parity was even over the
turns, not over the contests: harvest's beats are even (`turn_ticks` 1, `bank_ticks` 2, trees
every 30, 60, 90), so the ties that matter fall on one parity. The scripted gather-and-bank
policy with the world Rng re-seeded 60 ways at the first contest gave grove to the west 60 of
60 (to the east 60 of 60 with the parity inverted), gorge 23 west, 14 east, 23 drawn,
orchards and burrows 60 draws; the gatherer mirror (five a side, rotated, seed 1) went 16 to 3
for side 0 on grove in 20 games and 28 to 11 in 40 (p = 0.009), 100 to 92 in 200. With the
draw: the probe grove 23, 22, 15, gorge 18, 20, 22, orchards and burrows 60 draws; the
gatherer mirror in 40 games grove 15 to 23 (p = 0.26), orchards 18 to 16 (0.86), gorge 24 to
14 (0.14), burrows 16 to 19 (0.74), in 200 games 92 to 104 (0.43), 95 to 83 (0.41), 105 to 87
(0.22), 84 to 102 (0.21). Nothing is left lopsided to chase; the pinned asymmetries of §12
that remain (the dash exchange, the respawn order, the two Rng draws, the rounding) did not
show in these runs.
None of the design's balance targets is measured: they need step (4)'s house bots. Verified:
`tests/scenario/harvest_scenario.cpp`, `tree_scenario.cpp`, `grid_pathing_scenario.cpp`, the
baseline case of `powerup_scenario.cpp`, the harvest and moved control cases of
`side_symmetry_scenario.cpp` (a gather-and-bank harvest stays its own half turn bit for bit on
all four arenas until the world Rng first draws a tie, whose re-drawn outcomes pair up by the
half turn, and then plays on to its end: with inputs staggered by rank the first draw is a
fruit twins contend for, at tick 121 on grove, gorge and burrows and 181 on orchards, and with
every step at `subTick` 0 a tied cell between allies at tick 61 or 62; the control cases
stagger their inputs the same way, so the match without fights is mirrored bit for bit to its
draw, the banana-fight case plays seed 3 within 256 raw units and 64 BAM, where v0.80 said 64
and 4, and the dashing case seeds 4 and 69; the twins' swap case and a twins' pickup case pin
the two draws), `grid_scenario.cpp`'s draw cases (two contenders about half each, three about a
third, an earlier press winning with the Rng untouched, a swap, a swap with a third judged by
one order),
`tests/unit/ai/side_symmetry_test.cpp` (a slot's sight and observation under `own` and `team`
fog the half turn of its twin's on the pool arenas, and the reference bot's inputs mirrored for
one seed and mirrored observations), `contest_test.cpp` (each map's half turn, homes, trees,
capacities and reachability, both rows) and the loader's contest pin, the observation, SDK and
`example_bot_test` cases for the tree byte and the `gatherer`, `arena_test.cpp`'s pool cycle and
`arena` key, and the client's HUD, scoreboard, lobby, tuning, builder and theme tests. The
corpus was re-recorded for the content hash; checksums moved intentionally for the tied cell
contest's alternation (`scripted_10p_contest_1800t`, `scripted_10p_grid_rounds_1t_1800t`), for
the drawn pickup order (`scripted_10p_contest_1800t` 5 draws from tick 121,
`scripted_10p_contest_gorge_1800t` 11, `scripted_10p_contest_burrows_1800t` 4,
`scripted_10p_grid_rounds_1t_1800t` 1 at tick 119; the other 22 drew nothing) and for the drawn
cell contest (every entry with a tie: burrows 66 latches with a tie and 96 draws from tick 61,
`scripted_10p_control_1t_1800t` 14 and 38 from 63, `scripted_10p_grid_rounds_1t_1800t` 6 and 6
from 61, `scripted_4p_grid_rounds_1t_1800t` 2 and 4 from 556, and grove and gorge from 62 and 61
on their old input seeds, which they then changed to 0x5C12C1 and 0x5C12C2 to keep a theft and
a pit fall; `scripted_4p_grid_rounds_3600t` has no tie and no continuous entry moved). As
recorded, `scripted_10p_contest_1800t` plays harvest on `grove` (32 fruit taken, 53 banked, 13
stolen, team 0 winning 40 to 0 at tick 351); new `scripted_10p_contest_gorge_1800t` (46, 76, 21,
a pit fall, 40 to 15 at 475), `scripted_10p_contest_burrows_1800t` (56, 85, 23, team 1 40 to 22
at 573) and `scripted_10p_control_1t_1800t`, the old contest entry, byte-identical in its
checksums to it until the cell draw moved it; the harvest rows of `kfm_corpus_tests` expect
fruit taken off the floor, more banked than the ten opening hands and a theft. The finished
branch, on one Linux machine, the way CI runs it, in two passes. Before the drawn cell contest:
the lint job's three steps over the whole tree clean; a headless GCC 15 Debug build, 48 of 48
tests outside `soak` and `network`; the `dev` preset (GCC 15, Debug, the client), 58 of 58; the
`release` preset (GCC 15 at -O3 with `-Werror`, WebRTC on), 58 of 58 once three warnings were
fixed (e6c497f and 6e57c7a, `-Wnull-dereference` in the taking handlers and an observation test;
445a02d, `-Wsign-conversion` under ASan's flags in a test); `server-headless` (Release), 55 of
55 with the soak gates and the real-socket tests; `wasm` (Emscripten 6.0.13, node), the browser
client built and 51 of 51 under node; `asan` (address and undefined, no recovery), 55 of 55
outside `soak`. After it, on the final code: clang-format 23.1.3 over 405 files and clang-tidy
23.1.4 over 259 translation units against the `dev` compile database, no `error` or `warning`
line, and `check_sim_no_float.sh` clean; headless 48 of 48; `dev` 58 of 58; `release` 58 of 58,
no warning; `server-headless` 55 of 55; `asan` 55 of 55 outside `soak`; `wasm` 51 of 51 under
node. Not run: `client_smoke` (no `xvfb-run` on the machine; CI runs it), though a harvest match
on the shipped row has been looked at by hand (the homes, the trees and their fruit, the bunch
in hand, the HUD and the "Bananas" column read as built; a monkey on a tree is half hidden under
the canopy); the Windows, macOS and Clang legs; the Rust lobby job (no lobby code changed). Not
done and kept open in §11: the release; step (4) in the bots repository (the pin, the SDK's
trees and homes, the house bots `gatherer`, `raider` and `keeper`) and with it every balance
target; two teams reaching `score_to_win` on one tick draw (a `gatherer` mirror draws 2 to 11 %
of its games a map; a tie-break is a balance-patch question); a monkey on a tree half hidden
under the canopy; the reference bot does not gather, so it loses every game to `gatherer` and is
a weak opponent in human rooms of this kind; the capacities that bound the maps; the later
layers (spending, peels, bushes, escalation, a gate, homes that grant); the remaining pinned sim
asymmetries of §12 (the rounding, the dash exchange, the respawn order, the two Rng draws).

v0.83 changes: a ladder pair is one game on a drawn arena and drawn sides (§8 "The ladder"
"Matches and ratings", §11 "Scheduling and ratings" and the M7 row; §11 "Contest design:
bananas as the economy" records the release). Found while planning a contest bot: the match
job ran `kfm_arena --matches 2 --rotate`, and `kfm_arena` plays match k of a pooled mode on
member (k / T) mod P, so with two teams a two-game job never leaves member 0. Measured with
the released v0.10.0 runner: the ladder's command line, five `gatherer` a side, played `grove`
twice (786 and 732 ticks), and the live ladder's first three rounds on v0.10.0 (102 to 104)
recorded six games, every one on `grove`; 32 one-tick matches without `--arena` name the pool in its order (grove, grove,
orchards, orchards, gorge, gorge, burrows, burrows, and again). Chosen by the project's owner:
a random arena for each ladder match, and random sides, so a pair need not be played from both.
Decided with it: the draw comes from the job's seed by SHA-256 (the side from one bit, the
arena from eight bytes modulo the pool's size), so a retry records the same game and nothing
new is stored but the arena the result already names; the worker passes `--arena` and orders
the two `--team` lists, with no `--rotate`; the pool is asked of the runner (the one-tick
probe above, once per mode and worker) rather than configured in the service, which would be
a second copy of this repository's content; a runner without the `arena` key gets no
`--arena`. Considered and not taken: asking for 8 games a pair (every arena from both sides:
four times the compute and replays, and the owner asked for one game); rotating the arena by
round number (even, but predictable); a `kfm_arena` flag that draws the member itself (an
engine release for what the service can do with the documented `--arena`). Not changed: the
engine (no code, no content hash, no release), the pairing, the seed, Glicko-2 a game, the
validation smoke match (one game against `ref:hard` on the runner's default arena), `kfm
test`. Built in the bots repository as ladder API v1.6 and release v0.1.8: a match object gains
`arena`, `game` is 0 from there on, no migration. A pair now rates one game a round where it
rated two; `KFM_LADDER_ROUND_OPPONENTS` and the round interval are left as they are. Also
recorded here: v0.82's work is released as v0.10.0 (2026-11-13: the page, the VM, the
`kfm_arena` archives), the bots repository is pinned to it and the ladder runs it since its
v0.1.7; step (4) of the harvest order beyond the pin is not started. Verified in the bots
repository: its service's 190 tests and its CLI's 57 against Postgres 17, MinIO and the
released v0.10.0 `kfm_arena`, among them real games whose seeds draw each of the four arenas
from either side. Not done: the house bots for harvest and the balance targets; a tie-break
when both teams reach `score_to_win` on one tick.
