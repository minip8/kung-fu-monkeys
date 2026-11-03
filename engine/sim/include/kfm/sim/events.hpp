// SimEvents are the only channel from the sim to presentation, scoring
// and RL rewards (architecture §5.9). POD tagged union, fixed-capacity
// per-tick buffer.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/constants.hpp"

#include <array>
#include <cstdint>

namespace kfm::sim {

// Payload conventions (fields not listed are default):
enum class EventKind : uint8_t {
  None = 0,
  MatchStarted,       // -
  PlayerSpawned,      // player, entity
  RoundStarted,       // a = round
  RoundEnded,         // player = winner (invalid on a draw), a = round
  MatchEnded,         // player = winner (invalid on a draw)
  PlayerDashed,       // player, entity = player
  BoomerangThrown,    // player = owner, entity = boomerang
  BoomerangCaught,    // player = catcher, entity = boomerang, a = CatchHow
  BoomerangGrounded,  // entity = boomerang
  BoomerangBounced,   // entity = boomerang
  PlayerHit,          // player = victim, entity = boomerang, a = attacker slot, b = hp after
  PlayerEliminated,   // player = victim, a = killer slot or 0xFF
  PickupSpawned,      // entity = pickup, a = PickupKind, b = power-up index (PowerUp)
  PickupTaken,        // player, entity = pickup, a = PickupKind, b = power-up index (PowerUp)
  PlayerDashHit,      // player = victim, entity = attacker's player, a = attacker slot,
                      // b = hp after (a dash attack, §5.6; PlayerHit is a banana hit)
  // v0.23 (modes, §5.3):
  ScoreChanged,     // player, a = score after (a round win, or an elimination in deathmatch)
  PlayerRespawned,  // player, entity, a = spawn point index (deathmatch respawn or late join)
  TimerWarning,     // a = whole seconds left on the mode's clock (10, 3, 2, 1)
  ShieldBroken,     // player = shielded victim, entity = the banana or the dashing player,
                    // a = attacker slot, b = power-up index whose shield broke (§5.8)
};

// BoomerangCaught payload `a`.
enum class CatchHow : uint8_t { Perfect = 0, Auto = 1 };

struct SimEvent {
  EventKind kind{EventKind::None};
  Tick tick{};
  PlayerId player{PlayerId::invalid()};
  EntityId entity{};
  uint32_t a{0};  // kind-specific payload
  uint32_t b{0};
};

// Fixed-capacity buffer; extra events in one tick are counted and dropped.
struct EventSink {
  std::array<SimEvent, kMaxEventsPerTick> events{};
  uint16_t count{0};
  uint16_t dropped{0};

  constexpr void clear() {
    count = 0;
    dropped = 0;
  }
  constexpr void push(const SimEvent& e) {
    if (count < kMaxEventsPerTick) {
      events[count++] = e;
    } else {
      ++dropped;
    }
  }
  [[nodiscard]] constexpr const SimEvent* begin() const { return events.data(); }
  [[nodiscard]] constexpr const SimEvent* end() const { return events.data() + count; }
};

}  // namespace kfm::sim
