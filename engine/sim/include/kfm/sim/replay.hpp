// Replay format (architecture §9): seed + match config + content hash +
// the input stream. Encoded with BitWriter so it is a wire-format contract.
// The codec works on spans; callers own the buffers.
//
// Layout (all little-endian, bit-packed):
//   header : magic "KFMR" (4×u8), version u16, seed u64, contentHash u32,
//            buildHash u32, players u16, arena u8, mode u8, powerUps u16,
//            loadout u8, teams 10×u8, tickCount u32
//   (version 2, M4: arena and mode joined the header; version 3: the
//   per-match power-up set; version 4 (v0.31): the loadout; version 5
//   (v0.32): the team of every slot, each below kMaxPlayers. Older files are
//   refused and re-recorded by regen-replay-corpus)
//   frames : tickCount × InputFrame via visit() (present mask + inputs)
#pragma once

#include "kfm/core/bitstream.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace kfm::sim {

inline constexpr uint16_t kReplayVersion = 5;
inline constexpr size_t kReplayHeaderBytes =
    4 + 2 + 8 + 4 + 4 + 2 + 1 + 1 + 2 + 1 + kMaxPlayers + 4;

struct ReplayHeader {
  uint64_t seed{0};
  uint32_t contentHash{0};
  uint32_t buildHash{0};
  PlayerMask players{0};
  uint8_t arena{0};                 // MatchConfig::arena / ::mode: indices into the content
  uint8_t mode{0};                  // whose hash is contentHash
  uint16_t powerUps{kAllPowerUps};  // MatchConfig::powerUps
  uint8_t loadout{0};               // MatchConfig::loadout (version 4)
  TeamSet teams{kFreeForAllTeams};  // MatchConfig::teams (version 5)
  uint32_t tickCount{0};

  [[nodiscard]] constexpr MatchConfig matchConfig() const {
    return MatchConfig{seed, players, arena, mode, powerUps, loadout, teams};
  }
  // The config fields of a header (seed through teams; the hashes and the
  // tick count stay).
  constexpr void setMatchConfig(const MatchConfig& c) {
    seed = c.seed;
    players = c.players;
    arena = c.arena;
    mode = c.mode;
    powerUps = c.powerUps;
    loadout = c.loadout;
    teams = c.teams;
  }
};

// Upper bound on the encoded size of a replay with the given header.
[[nodiscard]] constexpr size_t replayMaxBytes(const ReplayHeader& h) {
  // Per frame: tick u32 + present u16 + up to kMaxPlayers × 5 bytes.
  return kReplayHeaderBytes + static_cast<size_t>(h.tickCount) * (4 + 2 + kMaxPlayers * 5);
}

void writeReplayHeader(BitWriter& w, const ReplayHeader& h);
// Returns false when the magic or version does not match or the data is short.
[[nodiscard]] bool readReplayHeader(BitReader& r, ReplayHeader& h);

void writeReplayFrame(BitWriter& w, const InputFrame& f);
[[nodiscard]] bool readReplayFrame(BitReader& r, InputFrame& f);

}  // namespace kfm::sim
