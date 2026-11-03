// EventJournal: SimEvents under rollback (architecture §5.9, §6.1).
//
// A predicting client steps some ticks more than once. The journal keeps the
// events of every tick that may still be re-simulated, keyed by (tick, hash
// of the payload), and applies the §5.9 rules:
//   - an event re-emitted by a re-simulation of its tick is a duplicate: it
//     is matched with the stored one and never delivered twice;
//   - an event of a re-simulated tick that the new timeline did not emit
//     belongs to the abandoned timeline: if it was not delivered yet it is
//     dropped, if it was it is retracted (counted; presentation cannot
//     un-play it);
//   - events of ticks at or before the final tick (confirm()) are final: a
//     re-simulation can no longer drop them.
// Equal events within one tick are matched by multiplicity.
//
// Usage per simulated tick t: beginTick(t), record() every event of the
// step, endTick(). drain() hands out the undelivered events in tick order.
//
// Storage is a fixed array kept sorted by tick (a re-simulation inserts
// within its tick's run), so a client holding the journal needs no heap.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/sim/events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace kfm::net {

struct EventJournalStats {
  uint64_t recorded{0};      // events offered by record()
  uint64_t delivered{0};     // handed out by drain()
  uint64_t deduplicated{0};  // re-emitted by a re-simulation, matched
  uint64_t dropped{0};       // abandoned timeline, never delivered
  uint64_t retracted{0};     // abandoned timeline, already delivered
  uint64_t overflow{0};      // journal full: event lost
};

class EventJournal {
 public:
  static constexpr size_t kCapacity = 2048;

  // Hash of the payload (kind, player, entity, a, b); the tick is the other
  // half of the key.
  [[nodiscard]] static uint32_t hashOf(const sim::SimEvent& e);

  // Starts recording tick t. Every stored event of t is "unseen" until the
  // new step emits it again.
  void beginTick(Tick t);
  // An event of the current tick (its own tick field is ignored).
  void record(const sim::SimEvent& e);
  // Ends the current tick: unseen events of it are dropped or retracted,
  // unless the tick is final.
  void endTick();

  // Ticks <= t are final. Delivered final events are forgotten.
  void confirm(Tick t);
  [[nodiscard]] std::optional<Tick> finalTick() const { return final_; }

  // Copies up to out.size() undelivered events, in tick order (then emission
  // order), into out, marks them delivered and returns how many.
  [[nodiscard]] size_t drain(std::span<sim::SimEvent> out);
  // Undelivered events still held.
  [[nodiscard]] size_t pending() const;

  [[nodiscard]] size_t size() const { return count_; }
  [[nodiscard]] const EventJournalStats& stats() const { return stats_; }
  void clear();

 private:
  struct Entry {
    sim::SimEvent event{};
    uint32_t hash{0};
    bool delivered{false};
    bool seen{false};
  };

  [[nodiscard]] bool isFinal(Tick t) const { return final_ && t <= *final_; }
  void eraseAt(size_t i);
  [[nodiscard]] bool evictOne();

  std::array<Entry, kCapacity> entries_{};
  size_t count_{0};
  // The run of the tick being recorded: [runBegin_, runEnd_) after the
  // events already matched or inserted.
  size_t runBegin_{0};
  size_t runEnd_{0};
  Tick current_{};
  bool inTick_{false};
  std::optional<Tick> final_;
  EventJournalStats stats_{};
};

static_assert(std::is_trivially_copyable_v<EventJournal>);

}  // namespace kfm::net
