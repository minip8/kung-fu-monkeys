#include "kfm/net/input_history.hpp"

#include "kfm/core/assert.hpp"

#include <algorithm>

namespace kfm::net {
namespace {

[[nodiscard]] constexpr bool validSlot(PlayerId slot) {
  return slot.isValid() && slot.value < sim::kMaxPlayers;
}

// True when t is too old to be stored next to `newest`.
[[nodiscard]] constexpr bool outOfWindow(Tick t, std::optional<Tick> newest) {
  return newest && static_cast<uint64_t>(t.value) + kInputHistoryTicks <= newest->value;
}

[[nodiscard]] constexpr uint32_t indexOf(Tick t) {
  return t.value & (kInputHistoryTicks - 1);
}

}  // namespace

// --- SlotInputHistory ----------------------------------------------------------

bool SlotInputHistory::put(Tick t, const sim::PlayerInput& in) {
  if (outOfWindow(t, newest_)) return false;
  entries_[indexOf(t)] = Entry{t, in, true};
  if (!newest_ || t > *newest_) newest_ = t;
  return true;
}

bool SlotInputHistory::has(Tick t) const {
  const Entry& e = entries_[indexOf(t)];
  return e.valid && e.tick == t;
}

std::optional<sim::PlayerInput> SlotInputHistory::get(Tick t) const {
  const Entry& e = entries_[indexOf(t)];
  if (e.valid && e.tick == t) return e.input;
  return std::nullopt;
}

sim::PlayerInput SlotInputHistory::getOrRepeat(Tick t, bool* exact) const {
  if (exact != nullptr) *exact = false;
  if (const auto in = get(t)) {
    if (exact != nullptr) *exact = true;
    return *in;
  }
  const uint32_t lookback = std::min<uint32_t>(t.value, kInputHistoryTicks - 1);
  for (uint32_t back = 1; back <= lookback; ++back) {
    if (const auto in = get(Tick{t.value - back})) return *in;
  }
  return sim::PlayerInput{};
}

void SlotInputHistory::clear() {
  entries_ = {};
  newest_.reset();
}

// --- InputHistory ----------------------------------------------------------------

bool InputHistory::put(PlayerId slot, Tick t, const sim::PlayerInput& in) {
  if (!validSlot(slot)) return false;
  return slots_[slot.value].put(t, in);
}

const SlotInputHistory& InputHistory::slot(PlayerId slot) const {
  KFM_CHECK(validSlot(slot));
  return slots_[slot.value];
}

void InputHistory::clearSlot(PlayerId slot) {
  if (validSlot(slot)) slots_[slot.value].clear();
}

sim::PlayerInput InputHistory::getOrRepeat(PlayerId slot, Tick t, bool* exact) const {
  if (!validSlot(slot)) {
    if (exact != nullptr) *exact = false;
    return sim::PlayerInput{};
  }
  return slots_[slot.value].getOrRepeat(t, exact);
}

void InputHistory::clear() {
  for (SlotInputHistory& s : slots_)
    s.clear();
}

InputMsg InputHistory::makeInputMsg(sim::PlayerMask slots, Tick newest, uint8_t count,
                                    std::optional<Tick> lastAuthTick) const {
  uint32_t n = std::clamp<uint32_t>(count, 1, kMaxInputRedundancy);
  n = std::min<uint32_t>(n, newest.value + 1);  // never before tick 0
  InputMsg m;
  m.count = static_cast<uint8_t>(n);
  m.startTick = Tick{newest.value + 1 - n};
  m.slots = static_cast<sim::PlayerMask>(slots & kAllPlayersMask);
  m.lastAuthTick = lastAuthTick;
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    const PlayerId id{static_cast<PlayerId::rep>(s)};
    if (!sim::maskHas(m.slots, id)) continue;
    for (uint32_t i = 0; i < n; ++i)
      m.inputs[s][i] = getOrRepeat(id, m.startTick.plus(static_cast<int32_t>(i)));
  }
  return m;
}

// --- FrameHistory ------------------------------------------------------------------

bool FrameHistory::put(const sim::InputFrame& f) {
  if (outOfWindow(f.tick, newest_)) return false;
  entries_[indexOf(f.tick)] = Entry{f, true};
  if (!newest_ || f.tick > *newest_) newest_ = f.tick;
  return true;
}

const sim::InputFrame* FrameHistory::get(Tick t) const {
  const Entry& e = entries_[indexOf(t)];
  return e.valid && e.frame.tick == t ? &e.frame : nullptr;
}

void FrameHistory::clear() {
  entries_ = {};
  newest_.reset();
}

std::optional<AuthInputsMsg> FrameHistory::makeAuthInputsMsg(Tick newest, uint8_t count) const {
  if (get(newest) == nullptr) return std::nullopt;
  const uint32_t want = std::clamp<uint32_t>(count, 1, kMaxInputRedundancy);
  uint32_t n = 1;
  while (n < want && n <= newest.value && get(Tick{newest.value - n}) != nullptr)
    ++n;
  AuthInputsMsg m;
  m.count = static_cast<uint8_t>(n);
  m.startTick = Tick{newest.value + 1 - n};
  for (uint32_t i = 0; i < n; ++i)
    m.frames[i] = *get(m.startTick.plus(static_cast<int32_t>(i)));
  return m;
}

// --- InputFrameAssembler -------------------------------------------------------------

InputFrameAssembler::PutResult InputFrameAssembler::put(PlayerId slot, Tick t,
                                                        const sim::PlayerInput& in) {
  if (!validSlot(slot)) return PutResult::BadSlot;
  if (t < next_) return PutResult::Late;
  if (t.minus(next_) >= kMaxLeadTicks) return PutResult::TooEarly;
  static_cast<void>(history_.put(slot, t, in));
  std::optional<Tick>& r = received_[slot.value];
  if (!r || t > *r) r = t;
  return PutResult::Accepted;
}

void InputFrameAssembler::clearSlot(PlayerId slot) {
  history_.clearSlot(slot);
  if (validSlot(slot)) received_[slot.value].reset();
}

sim::InputFrame InputFrameAssembler::assembleNext(sim::PlayerMask* substituted,
                                                  sim::PlayerMask* idled) {
  sim::InputFrame f;
  f.tick = next_;
  f.present = static_cast<sim::PlayerMask>(present_ & kAllPlayersMask);
  sim::PlayerMask missing = 0;
  sim::PlayerMask neutral = 0;
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    const PlayerId id{static_cast<PlayerId::rep>(s)};
    if (!sim::maskHas(f.present, id)) continue;
    bool exact = false;
    f.inputs[s] = history_.getOrRepeat(id, next_, &exact);
    if (!exact) {
      missing = static_cast<sim::PlayerMask>(missing | (1u << s));
      const std::optional<Tick>& r = received_[s];
      if (idleAfter_ > 0 && (!r || next_.minus(*r) > static_cast<int32_t>(idleAfter_))) {
        // Silent for longer than a loss burst: stand still, facing as before.
        const sim::PlayerInput last = f.inputs[s];
        f.inputs[s] = sim::PlayerInput{};
        f.inputs[s].moveDir = last.moveDir;
        f.inputs[s].aimDir = last.aimDir;
        neutral = static_cast<sim::PlayerMask>(neutral | (1u << s));
      }
      // The substitute is now the authoritative input of this tick.
      static_cast<void>(history_.put(id, next_, f.inputs[s]));
    }
  }
  if (substituted != nullptr) *substituted = missing;
  if (idled != nullptr) *idled = neutral;
  ++next_;
  return f;
}

void InputFrameAssembler::reset(Tick firstTick) {
  history_.clear();
  received_ = {};
  next_ = firstTick;
}

}  // namespace kfm::net
