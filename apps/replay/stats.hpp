// Feel-tuning statistics for kfm_replay (architecture §11 M2, §12): drives
// scripted matches and measures rounds, throws, hits, dashes, perfect-catch
// attempts and conversions, dash attacks, banana flight outcomes and time
// without ammo.
//
// Everything is read from SimEvents and WorldState, plus the per-tick contact
// list: statsStep() runs the §5.3 systems in step()'s order and lets the
// collector look at the contacts before InteractionSystem resolves them (the
// catch rule's inputs: the press time and each contact's tHit). Callers keep
// a shadow world advanced by step() and compare checksums every tick, so the
// statistics can never describe a pipeline that differs from step().
//
// Tooling, outside engine/sim: doubles are fine here.
#pragma once

#include "kfm/sim/contacts.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace kfm::replay {

// Counters and samples of one or more matches; merge() aggregates.
struct Stats {
  uint32_t matches{0};
  uint32_t matchesFinished{0};  // reached MatchEnded within the tick cap
  uint64_t ticks{0};
  uint64_t playingTicks{0};

  std::vector<double> roundSeconds;  // Playing start -> RoundEnded
  uint32_t draws{0};

  uint64_t throws{0};
  uint64_t hits{0};  // banana hits (PlayerHit)
  uint64_t eliminations{0};

  // Dash attacks (row 25, §5.6): dashes that touched a player the row accepts
  // (attacker Dashing, neither stunned nor eliminated, victim hittable), by
  // whether the attacker held content.dashAttack.minAmmo; then the hits
  // (PlayerDashHit) and the eliminations they caused.
  uint64_t dashAttackWithAmmo{0};
  uint64_t dashAttackWithoutAmmo{0};
  uint64_t dashHits{0};
  uint64_t dashEliminations{0};
  uint64_t dashes{0};
  uint64_t windowsOpened{0};  // rising edges of Tag::PerfectDash
  uint64_t perfectCatches{0};
  uint64_t perfectCatchesOwn{0};  // row 11: own returning banana
  uint64_t autoCatches{0};

  // Catch attempts: dashes whose PerfectDash window met a banana the catch
  // rows accept (rows 10/11 were consulted for at least one contact) while
  // the dasher had room below ammoMax.
  uint64_t attemptDashes{0};
  uint64_t convertedDashes{0};
  uint64_t consultedContacts{0};       // rows 10/11 consulted, below ammoMax
  uint64_t declinedContacts{0};        // ... outside [0, window): fell through
  std::vector<double> consultedDelta;  // tHit - pressTime (ticks) of those contacts
  // Rows 10/11 consulted with the dasher at ammoMax: the catch declines
  // (v0.16) and the contact falls through to damage or auto catch.
  uint64_t fullAmmoDeclined{0};
  uint64_t fullAmmoDeclinedInWindow{0};  // ... that would have been caught before v0.16

  // Danger contacts: a non-own banana in flight touching a live player.
  uint64_t dangerContacts{0};
  uint64_t dangerContactsRecentDash{0};  // the dash slot is busy or cooling down
  std::vector<double> dangerDelta;       // tHit - pressTime (ticks) of those

  // Flight outcome, seconds from the throw.
  std::vector<double> flightToPerfect;
  std::vector<double> flightToAuto;
  std::vector<double> flightToHit;
  std::vector<double> flightToGround;  // grounded without a hit (return timeout, owner gone)
  uint64_t flightUnresolved{0};        // still in the air when the round ended

  // Ammo.
  uint64_t aliveTicks{0};                 // live players x Playing ticks
  uint64_t emptyTicks{0};                 // ... of which at ammo 0
  std::vector<double> emptySpellSeconds;  // completed spells (ammo came back)
  uint64_t emptySpellsCensored{0};        // ended by elimination or round end

  // §12: perfect catches by ammo before the catch (v0.16: never at full ammo).
  uint64_t perfectAtFullAmmo{0};
  uint64_t perfectBelowFullAmmo{0};
  uint64_t autoAtFullAmmo{0};
  uint64_t eliminatedHoldingAmmo{0};  // the held banana vanishes with the player
  // §5.8 power-ups (arenas with spawners only).
  uint64_t powerUpsSpawned{0};
  uint64_t powerUpsTaken{0};
  uint64_t shieldsBroken{0};
  // Abilities in the third slot (v0.21).
  uint64_t blinks{0};
  uint64_t slams{0};
  uint64_t slammed{0};
  uint64_t parryPresses{0};
  uint64_t calls{0};  // v0.35: MonkeyCalled
  uint64_t parries{0};
  // Banana supply: ammo held + bananas in flight + pickups. With ammoMax = 1
  // a destroyed banana is gone for the round; at 0 with two or more players
  // alive nobody can ever be hit again and the round cannot end.
  uint32_t roundsSupplyExhausted{0};
  std::vector<double> roundMinSupplyShare;  // min supply / players at round start
  // The thrower whose banana was perfect-caught by someone else, by whether
  // the catcher was at full ammo: seconds until the thrower has ammo again,
  // or how that wait ended otherwise.
  struct ThrowerWait {
    std::vector<double> regainSeconds;
    uint64_t eliminatedFirst{0};
    uint64_t roundEndedFirst{0};
  };
  ThrowerWait afterFullCatch;
  ThrowerWait afterBelowFullCatch;

  void merge(const Stats& o);
};

// Watches one match. statsStep() feeds it beginContacts() once per tick and
// observeContact() for every contact InteractionSystem actually consults, in
// its resolution order and against the state at that moment; then
// afterStep() after every tick and finish() once.
class StatsCollector {
 public:
  // Records the pre-interaction state afterStep() compares against.
  void beginContacts(const sim::WorldState& s);
  // `content` is the match's (the one statsStep() steps with), so the
  // thresholds read are those of the loaded and selected content.
  void observeContact(const sim::WorldState& s, const sim::MatchContent& content,
                      const sim::Contact& c);
  void afterStep(const sim::WorldState& s, const sim::EventSink& sink);
  void finish(const sim::WorldState& s);
  [[nodiscard]] const Stats& stats() const { return stats_; }

 private:
  struct Wait {
    Tick since{};
    bool full{false};
  };
  void endRoundTracking(const sim::WorldState& s);

  Stats stats_;
  sim::MatchPhase prevPhase_{sim::MatchPhase::Lobby};
  Tick playingStart_{};
  std::array<bool, sim::kMaxPlayers> perfectTag_{};
  std::array<std::optional<Tick>, sim::kMaxPlayers> emptySince_{};
  std::array<std::optional<Wait>, sim::kMaxPlayers> throwerWait_{};
  uint32_t roundPlayers_{0};
  uint32_t roundMinSupply_{0};
  bool roundExhausted_{false};
  // This tick, filled by beginContacts and consumed by afterStep.
  std::array<uint8_t, sim::kMaxPlayers> ammoBefore_{};
  std::map<uint32_t, PlayerId> ownerBefore_;
  // Throw tick of every banana in flight, keyed by entity (index << 16 | generation).
  std::map<uint32_t, Tick> inFlight_;
  // Dashes (slot, press tick) with a consulted contact, and those that caught.
  std::set<std::pair<uint8_t, uint32_t>> attempted_;
  std::set<std::pair<uint8_t, uint32_t>> converted_;
  // Dashes (slot, press tick) that met a player row 25 accepts, by ammo.
  std::set<std::pair<uint8_t, uint32_t>> dashAttackWithAmmo_;
  std::set<std::pair<uint8_t, uint32_t>> dashAttackWithoutAmmo_;
};

// step() with the collector looking at the contact list (see above); like
// step()'s StepStats overload it reports what the tick's contact list
// dropped (overwritten, not summed).
void statsStep(sim::WorldState& s, const sim::InputFrame& in, const sim::MatchContent& content,
               sim::EventSink& sink, StatsCollector& collector, sim::StepStats& stepStats);

// One line per match.
void printMatchLine(uint32_t index, uint64_t seed, uint64_t inputSeed, const Stats& st);
// The aggregate block; the perfect window it labels is the dash of the
// match's loadout in `content` (the content the matches ran).
void printSummary(const Stats& st, uint32_t players, const sim::MatchContent& content);

}  // namespace kfm::replay
