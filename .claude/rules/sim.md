---
paths:
  - "engine/sim/**"
  - "engine/content/**"
  - "content/**"
  - "apps/replay/**"
  - "tests/scenario/**"
  - "tests/determinism/**"
  - "tests/unit/sim/**"
  - "tests/unit/content/**"
  - "tools/replay-corpus/**"
---

# Sim and content rules

These add to the determinism contract and the tables in `CLAUDE.md` ("Architecture in one
screen"), which always apply.

- Teams never enter `WorldState`: a free-for-all config (slot i = team i) must keep every
  checksum. New rows that hurt or push get an Enemy row and an Ally twin needing
  `Tag::FriendlyFire` on the receiving monkey (a test pins the twins); hosts fill
  `MatchConfig::teams` with `sim::teamsFor(mode, present)` (or through `resolveMatchNames`).
  There is no friendly fire unless the mode's `baseline` power-up grants tag `friendly_fire`.
- Objective kinds keep their state in `MatchRules::modeData` (layouts in `src/modes/hill.cpp`,
  `capture.cpp` and `docs/bot-abi.md` §3), never in a new world-state field; a capture flag is a
  `PickupKind::Flag` pickup whose `spawner` byte is its team. The scripted generator's objective
  steering must stay Rng-free and keyed on the mode kind, or every other corpus entry moves.
- Spaces: how monkeys move is `spaceBehaviorFor(mode.space)` (`src/spaces/`), nothing else;
  the continuous row must stay the sim as built bit for bit. Cells come only from
  `kfm/sim/spaces/grid_cells.hpp` (column 0 west, row 0 north, a cell centre = a tilemap spawn
  point). The grid latch reads the frame and the world state only (rollback-exact); contested
  cells are not rule rows. Grid content counts in turns: a grid throw's recovery equals
  `turn_ticks` and roots, a grid dash's active ticks are the slide; a mode's numbers that must
  scale with the turn need their own defs (the `_1t` twins). The contact radius is
  `Stats::radius`, never `PlayerDef::radius`, in sim code.
- Pits: a pit starts a fall (`Tag::Falling`, `Player::fallTicks` from `[player]
  fall_ticks`, `PlayerFell`), never an elimination on the spot, and only once the whole contact
  disc is over the union of the pit boxes (`discOverPits` / `discWithinBoxes` in
  `content/arena.hpp`, exact and integer-only; the bot uses the same function). Row 60 forbids
  `Dashing`, so a dash's reach is how wide a pit it clears. `kGone` in `rules/table.cpp` holds
  `Falling`: a new row that forbids `kGone` is safe against a falling monkey.
- Bananas never disappear: a dead (or newly falling) monkey's bananas in flight are released at
  once (`releaseBananasOf`: dropped as pickups, or gone in a mode whose `death_bananas` is
  vanish), and held ones at a non-pit death in a drop mode, a catch past `ammoMax` and a full
  pickup pool all drop or stack (`rules/bananas.hpp`). A new path that would destroy a banana
  must drop it instead; the exceptions are a vanish mode's death and an ammo power-up's extra
  bananas when it runs out. A grounded banana wholly over a pit sinks instead
  (`BoomerangPhase::Sinking`, never a pickup); a new Boomerang row must name an
  Outbound/Returning phase mask so it never touches a sinking one (a test pins it).
- A new round-ending rule must wait for falls (`anyFalling`), or a doomed last monkey wins; the
  round kinds also end a dry round (no banana left anywhere, `roundRanDry`, `[[mode]]
  dry_round_ticks`), and anything new that makes bananas must count in `bananaLeft`.
- Serialised structs: every `visit()` overload in `kfm/sim/visit.hpp` is followed by a
  `static_assert(fieldCount<T>() == n)`; a new field means the visitor line, the count and a
  mutation in `visit_test.cpp` / `content_test.cpp` (the tests pin their count the same way).
  The scenario tests' hand copies of shipped defs are checked against the TOML by
  `tests/unit/content/mirror_test.cpp`: edit both.
- A new mode hook or test-only mode: `MatchContent::modeBehavior` swaps the kind's
  `ModeBehavior` row in a scenario (`tests/scenario/mode_hooks_scenario.cpp`); hosts never set it.
- Spawn groups fixed the side bias between teams only: the free-for-all spawn-order slot bias
  of §12 is still open (tournaments rotate).
- The `contest` row of `content/modes.toml` is the bot ladder's: a loader test pins it, so a
  season's change to it is a deliberate edit of both.
