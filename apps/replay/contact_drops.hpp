// Contact-list overflow over a run of kfm_replay (issue #70): the per-tick
// sim::StepStats::contactsDropped (v0.70) summed by bench, verify and stats.
// The contact list holds kMaxContacts; what a dense tick pushes past that is
// dropped in pool-index order (deterministic, but a hit, catch or pickup
// that silently did not happen), so the tools say when it happens.
#pragma once

#include "kfm/core/types.hpp"

#include <cstdint>
#include <string>

namespace kfm::replay {

struct ContactDrops {
  uint64_t total{0};  // contacts dropped, summed over every tick
  uint64_t ticks{0};  // ticks that dropped at least one
  // The tick that dropped the most (the earliest on a tie): the stepped
  // tick (the input frame's), and the match it belongs to where a command
  // runs several (match k as the command numbers them; 0 otherwise).
  uint16_t worst{0};
  Tick worstTick{};
  uint32_t worstMatch{0};

  // One stepped tick and what it dropped (0 is counted nowhere).
  void add(Tick tick, uint16_t dropped, uint32_t match = 0);
  // Another run's drops after this one's (a later match keeps an earlier tie).
  void merge(const ContactDrops& other);
  [[nodiscard]] bool any() const { return total != 0; }
};

// The report line, newline included, or "" when nothing was dropped:
//   warning: contact list overflow (kMaxContacts 256): 1165 contacts dropped
//   over 3 ticks, worst tick 812 with 409
// on one line; `perMatch` names the worst tick's match ("worst match 2 tick
// 812"), `fatal` starts it with "error:" (verify --fail-on-dropped-contacts).
[[nodiscard]] std::string contactDropsLine(const ContactDrops& drops, bool perMatch,
                                           bool fatal = false);

// Writes contactDropsLine() to stderr (nothing when nothing was dropped).
void reportContactDrops(const ContactDrops& drops, bool perMatch, bool fatal = false);

}  // namespace kfm::replay
