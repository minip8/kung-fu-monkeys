#include "kfm/net/event_journal.hpp"

#include "kfm/core/assert.hpp"
#include "kfm/core/hash.hpp"

#include <algorithm>

namespace kfm::net {
namespace {

[[nodiscard]] bool samePayload(const sim::SimEvent& a, const sim::SimEvent& b) {
  return a.kind == b.kind && a.player == b.player && a.entity == b.entity && a.a == b.a &&
         a.b == b.b;
}

}  // namespace

uint32_t EventJournal::hashOf(const sim::SimEvent& e) {
  Hash32 h;
  h.add(uint32_t{static_cast<uint8_t>(e.kind)});
  h.add(uint32_t{e.player.value});
  h.add((uint32_t{e.entity.index} << 16) | e.entity.generation);
  h.add(e.a);
  h.add(e.b);
  return h.finish();
}

void EventJournal::beginTick(Tick t) {
  KFM_ASSERT(!inTick_);
  // Entries are sorted by tick and the recorded tick is almost always at the
  // end, so search backwards.
  size_t end = count_;
  while (end > 0 && entries_[end - 1].event.tick > t)
    --end;
  size_t begin = end;
  while (begin > 0 && entries_[begin - 1].event.tick == t)
    --begin;
  for (size_t i = begin; i < end; ++i)
    entries_[i].seen = false;
  runBegin_ = begin;
  runEnd_ = end;
  current_ = t;
  inTick_ = true;
}

void EventJournal::record(const sim::SimEvent& e) {
  KFM_ASSERT(inTick_);
  ++stats_.recorded;
  const uint32_t h = hashOf(e);
  for (size_t i = runBegin_; i < runEnd_; ++i) {
    Entry& x = entries_[i];
    if (!x.seen && x.hash == h && samePayload(x.event, e)) {
      x.seen = true;
      ++stats_.deduplicated;
      return;
    }
  }
  if (count_ == kCapacity && !evictOne()) {
    ++stats_.overflow;
    return;
  }
  for (size_t i = count_; i > runEnd_; --i)
    entries_[i] = entries_[i - 1];
  Entry& n = entries_[runEnd_];
  n.event = e;
  n.event.tick = current_;
  n.hash = h;
  n.delivered = false;
  n.seen = true;
  ++runEnd_;
  ++count_;
}

void EventJournal::endTick() {
  KFM_ASSERT(inTick_);
  inTick_ = false;
  if (isFinal(current_)) return;
  size_t i = runBegin_;
  while (i < runEnd_) {
    if (entries_[i].seen) {
      ++i;
      continue;
    }
    if (entries_[i].delivered) {
      ++stats_.retracted;
    } else {
      ++stats_.dropped;
    }
    eraseAt(i);
    --runEnd_;
  }
}

void EventJournal::confirm(Tick t) {
  if (final_ && t <= *final_) return;
  final_ = t;
  size_t out = 0;
  for (size_t i = 0; i < count_; ++i) {
    const Entry& e = entries_[i];
    if (e.delivered && e.event.tick <= t) continue;
    entries_[out++] = e;
  }
  count_ = out;
}

size_t EventJournal::drain(std::span<sim::SimEvent> out) {
  KFM_ASSERT(!inTick_);
  size_t n = 0;
  for (size_t i = 0; i < count_ && n < out.size(); ++i) {
    Entry& e = entries_[i];
    if (e.delivered) continue;
    out[n++] = e.event;
    e.delivered = true;
    ++stats_.delivered;
  }
  if (final_) {
    // Forget what is now delivered and final.
    const Tick t = *final_;
    final_.reset();
    confirm(t);
  }
  return n;
}

size_t EventJournal::pending() const {
  return static_cast<size_t>(std::count_if(entries_.begin(), entries_.begin() + count_,
                                           [](const Entry& e) { return !e.delivered; }));
}

void EventJournal::clear() {
  count_ = 0;
  runBegin_ = runEnd_ = 0;
  inTick_ = false;
  final_.reset();
}

void EventJournal::eraseAt(size_t i) {
  for (size_t j = i + 1; j < count_; ++j)
    entries_[j - 1] = entries_[j];
  --count_;
}

bool EventJournal::evictOne() {
  // The oldest delivered entry outside the run being recorded; it can only
  // be needed again to recognise a duplicate, which then is delivered twice.
  for (size_t i = 0; i < count_; ++i) {
    if (i >= runBegin_ && i < runEnd_) continue;
    if (!entries_[i].delivered) continue;
    eraseAt(i);
    if (i < runBegin_) {
      --runBegin_;
      --runEnd_;
    }
    return true;
  }
  return false;
}

}  // namespace kfm::net
