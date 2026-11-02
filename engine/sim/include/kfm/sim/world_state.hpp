// The complete simulation state (architecture §5.2). Trivially copyable:
// snapshot save/restore for rollback is a memcpy. Everything the sim needs
// to advance one tick is here or in the immutable content.
//
// M0 carries the skeleton only: tick, RNG, match rules and a player pool
// with transform. M1 fills in the entity zoo.
#pragma once

#include "kfm/core/pool.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"

#include <cstdint>
#include <type_traits>

namespace kfm::sim {

enum class MatchPhase : uint8_t {
  Lobby = 0,
  Playing,
  Ended,
};

struct MatchRules {
  MatchPhase phase{MatchPhase::Lobby};
  PlayerMask present{0};
  uint32_t ticksInPhase{0};
};

struct Transform {
  Vec2 pos{};
  Vec2 vel{};
  Angle facing{};
};

struct Player {
  PlayerId id{PlayerId::invalid()};
  Transform transform{};
};

struct WorldState {
  Tick tick{};
  Rng rng{};
  MatchRules rules{};
  Pool<Player, kMaxPlayers> players{};
  // M1: boomerangs, pickups, hazards.

  // Players are indexed by slot: the Pool index for a present player equals
  // its PlayerId, which keeps the mapping trivial and deterministic.
  [[nodiscard]] const Player* playerBySlot(PlayerId id) const {
    if (!id.isValid() || id.value >= kMaxPlayers) return nullptr;
    const auto index = static_cast<uint16_t>(id.value);
    return players.isAliveIndex(index) ? &players.items[index] : nullptr;
  }
};
static_assert(std::is_trivially_copyable_v<WorldState>);

// Match configuration that seeds a fresh world.
struct MatchConfig {
  uint64_t seed{0};
  PlayerMask players{0};
};

// Builds the initial state for a match: tick 0, seeded RNG, lobby phase.
[[nodiscard]] WorldState initialWorld(const MatchConfig& config);

}  // namespace kfm::sim
