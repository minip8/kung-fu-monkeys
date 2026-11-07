// The reference bot (architecture §8): a utility AI over the Observation
// alone, never the WorldState, so the same logic is writable as an SDK bot
// and is the worked example in each SDK language. It fills lobby slots in
// the shipped game and is the baseline a contest bot must beat.
//
// Each tick it predicts every enemy banana in flight over a short horizon
// (the sim's flight rule: outbound, then steering home at the turn rate,
// bouncing off the outer box) and scores:
//   - movement: 16 stick directions and idle, each walked over the horizon
//     against the predicted bananas (a contact is a heavy penalty, a near
//     miss a light one), plus the goal of the moment: keep a throwing range
//     to the target, with a clear line past the walls (v0.57 in every space),
//     with a banana in hand; empty-handed, meet the own banana
//     on its way back, else fetch the nearest banana lying around (v0.57:
//     not one deep in a pit); power-ups close by; stay off the outer walls;
//     and (v0.57) a path that ends a tick with its whole disc over the pits
//     outside a dash's active ticks is a fall, penalised like a hit;
//   - dash-catch: with room for a banana, a dash whose perfect window meets
//     an enemy banana (§5.6, §5.11);
//   - dodge: when no walk escapes a hit, the safest dash, else the third
//     slot's blink, parry or slam when the loadout has one;
//   - dash attack: holding the ammo that arms it, a dash that meets the
//     target's predicted path, only on a target that cannot strike back on
//     the same tick (unarmed, or its dash busy or cooling down): two strikes
//     pressed on one tick are resolved in slot order, a coin tossed by slot
//     index rather than a fight;
//   - pit crossing (v0.57, continuous space): when the walk the goal wants
//     most, pits aside, would fall in, a dash the same way that lands clear
//     of every pit (and of every banana) and scores better takes it;
//   - throw: a lead shot (intercept of the target's velocity at banana
//     speed, after the wind-up and the host delay) within the outbound range
//     and clear of the walls; in a standoff (the target can throw back at
//     once) held for a random 0..2 ticks, drawn once per lined-up shot.
// Fairness: nothing in the evaluation depends on the bot's own slot, and
// ties (equal target distances, equal movement scores) go to a random
// candidate, never to the lowest slot, banana or direction index. With no
// noise a bot is otherwise a pure function of the world, so mirrored hard
// bots would play mirrored rounds (the same opening for every spawn
// rotation, both falling on one tick); the tie-breaks and the standoff hold
// are where the per-slot seed breaks that symmetry without a handicap.
// On the grid (v0.35: match.reservedTurnTicks > 0) it walks cells: the
// eight octants and idle, each a slide of one cell per turn from the next
// latch (match.reservedTicksToLatch) on; dashes go along octants; it fights
// at close range (two cells: a banana from farther is side-stepped, and
// hearing warns of one from behind), its throw range capped at three cells
// and its flight speed learned from the bananas it sees (Observation v1
// carries only the base speed); the catcher's dash catches as any dash
// with a perfect window. The cell size it infers from its own cell while at
// rest. Under fog it acts only on what it sees (zeroed entries are never
// monkeys or bananas): with no enemy in sight it heads for the last one it
// saw or an ally called out, else the arena's centre, and turns its cone
// toward that goal (sweeping a quarter turn every four turns once there).
// With a call on the ability button it calls out a seen enemy's coarse
// cell, symbol = (col / 2) | (row / 2) << 4, at most once per eight turns;
// a call on a latch tick also sets the facing to the symbol for that turn,
// the price of talking.
// A monkey falling down a pit (v0.57, Tag::Falling) is no target and no
// threat; falling itself, the bot does nothing.
// It aims ahead by the observation's inputDelayTicks, presses buttons only
// as fresh edges (never two ticks in a row) at sub-tick 0, and is
// deterministic: fixed-point only, its own Rng seeded by init(), no
// allocation after construction.
#pragma once

#include "kfm/ai/agent.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/input.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace kfm::ai {

inline constexpr uint8_t kMaxReactionDelayTicks = 30;

// Difficulty (casual lobbies only: a contest bot never gets a handicap, §8).
struct ReferenceBotConfig {
  // Ticks between deciding and pressing (applied on top of the host delay,
  // and not compensated: the bot reacts late). At most kMaxReactionDelayTicks.
  uint8_t reactionDelayTicks{0};
  // ± random aim error of a throw, in 1/256 turns.
  uint8_t aimNoise{0};
  // Chance in 256 per tick to take a dash-catch that would connect (256: always).
  uint16_t catchPer256{256};
  // Chance in 256 per tick to throw (or dash-attack) once the shot is lined up.
  uint16_t throwPer256{256};

  [[nodiscard]] static constexpr ReferenceBotConfig easy() { return {18, 12, 48, 16}; }
  [[nodiscard]] static constexpr ReferenceBotConfig medium() { return {8, 5, 128, 64}; }
  // No delay, no noise: the baseline contest bots are measured against.
  [[nodiscard]] static constexpr ReferenceBotConfig hard() { return {0, 0, 256, 256}; }

  constexpr bool operator==(const ReferenceBotConfig&) const = default;
};

class ReferenceBot final : public IAgent {
 public:
  explicit ReferenceBot(const ReferenceBotConfig& config = ReferenceBotConfig::hard());

  void init(uint64_t seed, PlayerId slot) override;
  std::optional<sim::PlayerInput> act(const Observation& obs, Tick tick) override;

  [[nodiscard]] const ReferenceBotConfig& config() const { return config_; }

  // The prediction horizon in ticks and the most bananas tracked at once.
  static constexpr uint8_t kHorizon = 32;
  static constexpr uint8_t kMaxTracks = 12;

 private:
  // A predicted path: pos[k] = the centre at the end of the k-th tick from
  // now (pos[0] = now).
  struct Track {
    std::array<Vec2, kHorizon + 1> pos{};
    Fixed radius{};
    // The first tick of pos[] that can still hit (a banana grounding stops
    // being harmful); kHorizon + 1 when it never stops.
    uint8_t harmlessFrom{kHorizon + 1};
    bool catchable{false};  // a banana (a dashing monkey is a track too, never caught)
  };
  // A path of the own monkey under a plan, pos[k] as in Track.
  struct Path {
    std::array<Vec2, kHorizon + 1> pos{};
  };
  // What a path meets: the earliest contact tick (0 = none) and, for a dash,
  // whether it is a catch.
  struct Outcome {
    Fixed danger{};
    uint8_t firstHit{0};
    bool catches{false};
  };

  [[nodiscard]] sim::PlayerInput decide(const Observation& obs);
  void trackThreats(const Observation& obs);
  static void predictBanana(const Observation& obs, const ObsBanana& b, Track& t);
  static void walkPath(const Observation& obs, uint8_t dir, bool idle, Path& p);
  // v0.35: a grid walk along an octant (idle: stay) from the next latch on.
  void gridWalkPath(const Observation& obs, uint8_t octant, bool idle, Path& p) const;
  // v0.35: what the grid and the fog let the bot learn and remember.
  void learn(const Observation& obs, int target);
  // v0.35: the grid's free cells (for the own circle), once per match, and
  // the walking distance in cells from every cell to `goal` (BFS over the
  // eight neighbours without corner cutting, as the grid steps; a pit is
  // never free: the sim lets a walk into one, the bot does not).
  void prepareGrid(const Observation& obs);
  void distancesTo(Vec2 goal);
  [[nodiscard]] int32_t cellIndexOf(Vec2 at) const;
  // The walking distance from `at` to the goal of the last distancesTo(), in
  // units (unreachable: across the whole grid).
  [[nodiscard]] Fixed walkDistance(Vec2 at) const;
  static void dashPath(const Observation& obs, uint8_t dir, Path& p);
  static void blinkPath(const Observation& obs, uint8_t dir, Path& p);
  // Scores a path against the tracks. Ticks [catchFrom, catchUntil) of the
  // path (as in Track::pos) are a dash's perfect window: a banana met there is
  // caught, not a hit (catchUntil 0 = no window).
  [[nodiscard]] Outcome evaluate(const Observation& obs, const Path& p, uint8_t catchFrom,
                                 uint8_t catchUntil) const;
  // v0.57: the penalty of a path that falls down a pit: the first tick whose
  // whole disc is over the pits (the sim's test) outside [airFrom, airUntil)
  // (a dash's active ticks, which carry the monkey over; airUntil 0 = none),
  // weighted like a hit (sooner is worse); 0 when it never does.
  [[nodiscard]] static Fixed pitDanger(const Observation& obs, const Path& p, uint8_t airFrom,
                                       uint8_t airUntil);

  ReferenceBotConfig config_;
  Rng rng_{};
  PlayerId slot_{PlayerId::invalid()};
  uint8_t lastDecidedButtons_{0};
  uint8_t lastMoveDir_{0};
  uint32_t decisions_{0};
  // Ticks left of a standoff's throw hold; -1 while none is drawn.
  int8_t throwHold_{-1};
  // v0.48, a charging throw: the ticks the button stays held after the press
  // tick (0 = none or a tap), and the planned flight speed (Q16.16 per tick),
  // which keeps the aim leading the target until the release.
  uint16_t chargeHold_{0};
  Fixed chargeSpeed_{};
  // The reaction delay line: decision n is played on act n + reactionDelayTicks.
  std::array<sim::PlayerInput, kMaxReactionDelayTicks + 1> pending_{};
  // Scratch, kept here rather than on the stack (no allocation after construction).
  std::array<Track, kMaxTracks> tracks_{};
  uint8_t trackCount_{0};
  Path path_{};
  // v0.35 (grid, fog): the inferred cell size, the learned banana flight
  // speed (0 = none seen yet), the last place an enemy was seen or called
  // out (and when), the tick of the last call.
  Fixed cellSize_{Fixed::one()};
  Fixed bananaSpeed_{};
  Vec2 lastSeen_{};
  uint32_t lastSeenTick_{0};
  bool haveLastSeen_{false};
  uint32_t lastCallTick_{0};
  bool called_{false};
  // The grid map (prepareGrid, distancesTo); 64 x 64 cells at most, else no
  // map (gridCols_ 0) and plain distances.
  static constexpr int32_t kMaxGridCells = 64 * 64;
  std::array<uint8_t, kMaxGridCells> gridFree_{};
  std::array<uint16_t, kMaxGridCells> gridDist_{};
  std::array<uint16_t, kMaxGridCells> gridQueue_{};
  int32_t gridCols_{0};
  int32_t gridRows_{0};
  Fixed gridCell_{};      // the cell size the map was built for (0: none)
  int32_t distGoal_{-1};  // the cell gridDist_ measures to
};

}  // namespace kfm::ai
