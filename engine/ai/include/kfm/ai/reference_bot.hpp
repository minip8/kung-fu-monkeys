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
//     to the target with a banana in hand; empty-handed, meet the own banana
//     on its way back, else fetch the nearest banana lying around; power-ups
//     close by; stay off the outer walls;
//   - dash-catch: with room for a banana, a dash whose perfect window meets
//     an enemy banana (§5.6, §5.11);
//   - dodge: when no walk escapes a hit, the safest dash, else the third
//     slot's blink, parry or slam when the loadout has one;
//   - dash attack: holding the ammo that arms it, a dash that meets the
//     target's predicted path, only on a target that cannot strike back on
//     the same tick (unarmed, or its dash busy or cooling down): two strikes
//     pressed on one tick are resolved in slot order, a coin tossed by slot
//     index rather than a fight;
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
  static void dashPath(const Observation& obs, uint8_t dir, Path& p);
  static void blinkPath(const Observation& obs, uint8_t dir, Path& p);
  // Scores a path against the tracks. Ticks [catchFrom, catchUntil) of the
  // path (as in Track::pos) are a dash's perfect window: a banana met there is
  // caught, not a hit (catchUntil 0 = no window).
  [[nodiscard]] Outcome evaluate(const Observation& obs, const Path& p, uint8_t catchFrom,
                                 uint8_t catchUntil) const;

  ReferenceBotConfig config_;
  Rng rng_{};
  PlayerId slot_{PlayerId::invalid()};
  uint8_t lastDecidedButtons_{0};
  uint8_t lastMoveDir_{0};
  uint32_t decisions_{0};
  // Ticks left of a standoff's throw hold; -1 while none is drawn.
  int8_t throwHold_{-1};
  // The reaction delay line: decision n is played on act n + reactionDelayTicks.
  std::array<sim::PlayerInput, kMaxReactionDelayTicks + 1> pending_{};
  // Scratch, kept here rather than on the stack (no allocation after construction).
  std::array<Track, kMaxTracks> tracks_{};
  uint8_t trackCount_{0};
  Path path_{};
};

}  // namespace kfm::ai
