// Input bookkeeping shared by NetClient and NetServer (architecture §6.1,
// §6.2). All of it is fixed-capacity, allocation-free and trivially
// copyable, so a client can keep it next to its saved states.
//
//   SlotInputHistory   the inputs of one player slot by tick, the last
//                      kInputHistoryTicks ticks; "repeat last input" lookup.
//   InputHistory       one SlotInputHistory per slot, plus the redundant
//                      window an InputMsg resends.
//   FrameHistory       authoritative InputFrames by tick (the server resends
//                      the last 16 in AuthInputs; the client keeps confirmed
//                      frames for reconciliation).
//   InputFrameAssembler  the server side of §6.1: collects inputs for future
//                      ticks, assembles InputFrame(T) when T is due,
//                      substituting "repeat last input" for whatever has not
//                      arrived, and refuses inputs for ticks already final.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace kfm::net {

// Covers the rollback window (32 ticks), the redundancy window (16) and the
// lead (§6.2) with room to spare. A power of two so tick → index is a mask.
inline constexpr uint32_t kInputHistoryTicks = 128;
static_assert((kInputHistoryTicks & (kInputHistoryTicks - 1)) == 0);

class SlotInputHistory {
 public:
  // Stores the input of tick t. A tick older than newest() −
  // kInputHistoryTicks + 1 no longer fits and is ignored (returns false);
  // a newer tick evicts what it overwrites.
  bool put(Tick t, const sim::PlayerInput& in);

  [[nodiscard]] bool has(Tick t) const;
  [[nodiscard]] std::optional<sim::PlayerInput> get(Tick t) const;

  // The input known for t, else the latest known input of a tick before t
  // ("repeat last input", §6.1), else idle. `exact` reports which.
  [[nodiscard]] sim::PlayerInput getOrRepeat(Tick t, bool* exact = nullptr) const;

  [[nodiscard]] std::optional<Tick> newest() const { return newest_; }
  void clear();

 private:
  struct Entry {
    Tick tick{};
    sim::PlayerInput input{};
    bool valid{false};
  };
  std::array<Entry, kInputHistoryTicks> entries_{};
  std::optional<Tick> newest_;
};

class InputHistory {
 public:
  // Invalid slots are ignored (false).
  bool put(PlayerId slot, Tick t, const sim::PlayerInput& in);
  // Precondition: slot < kMaxPlayers.
  [[nodiscard]] const SlotInputHistory& slot(PlayerId slot) const;
  void clearSlot(PlayerId slot);
  [[nodiscard]] sim::PlayerInput getOrRepeat(PlayerId slot, Tick t, bool* exact = nullptr) const;
  void clear();

  // The InputMsg resending the last `count` ticks ending at `newest`
  // (inclusive) for every slot in `slots`; ticks before 0 are skipped, ticks
  // the history no longer has are sent as repeat-last. count is clamped to
  // 1..kMaxInputRedundancy.
  [[nodiscard]] InputMsg makeInputMsg(sim::PlayerMask slots, Tick newest, uint8_t count,
                                      std::optional<Tick> lastAuthTick) const;

 private:
  std::array<SlotInputHistory, sim::kMaxPlayers> slots_{};
};

class FrameHistory {
 public:
  // Stores frame f under f.tick; ignored (false) when older than the window.
  bool put(const sim::InputFrame& f);
  [[nodiscard]] const sim::InputFrame* get(Tick t) const;
  [[nodiscard]] std::optional<Tick> newest() const { return newest_; }
  void clear();

  // The AuthInputsMsg carrying the last `count` frames ending at `newest`
  // (inclusive), clamped to the frames held contiguously; nullopt when the
  // history does not hold `newest`.
  [[nodiscard]] std::optional<AuthInputsMsg> makeAuthInputsMsg(Tick newest, uint8_t count) const;

 private:
  struct Entry {
    sim::InputFrame frame{};
    bool valid{false};
  };
  std::array<Entry, kInputHistoryTicks> entries_{};
  std::optional<Tick> newest_;
};

class InputFrameAssembler {
 public:
  enum class PutResult : uint8_t {
    Accepted = 0,
    Late,      // tick already assembled: the authoritative frame used a substitute
    TooEarly,  // more than kMaxLeadTicks ahead of the next tick to assemble
    BadSlot,   // not a player slot
  };

  // Ticks ahead of nextTick() an input may arrive (half the history, so the
  // repeat-last lookback never meets a wrapped entry).
  static constexpr int32_t kMaxLeadTicks = kInputHistoryTicks / 2;

  explicit InputFrameAssembler(Tick firstTick = Tick{0}) : next_(firstTick) {}

  // Present slots of the frames assembled from now on.
  void setPresent(sim::PlayerMask present) { present_ = present; }
  [[nodiscard]] sim::PlayerMask present() const { return present_; }

  // Stores the input of any slot for a tick in [nextTick(), nextTick() +
  // kMaxLeadTicks); whether the slot is present is decided at assembly, and
  // whether the sender owns it is the caller's check (§6.7).
  PutResult put(PlayerId slot, Tick t, const sim::PlayerInput& in);

  // Forgets a slot's inputs (a player left; the next one starts idle).
  void clearSlot(PlayerId slot) { history_.clearSlot(slot); }

  // Assembles nextTick() and advances it. `substituted` (optional) gets the
  // present slots whose input had not arrived and was repeated. The frame is
  // recorded as the authoritative stream: later lookups repeat from it.
  [[nodiscard]] sim::InputFrame assembleNext(sim::PlayerMask* substituted = nullptr);

  [[nodiscard]] Tick nextTick() const { return next_; }
  [[nodiscard]] const InputHistory& history() const { return history_; }

  // Restarts at `firstTick` (a new match), clearing every input.
  void reset(Tick firstTick);

 private:
  InputHistory history_{};
  sim::PlayerMask present_{0};
  Tick next_{};
};

static_assert(std::is_trivially_copyable_v<SlotInputHistory>);
static_assert(std::is_trivially_copyable_v<InputHistory>);
static_assert(std::is_trivially_copyable_v<FrameHistory>);
static_assert(std::is_trivially_copyable_v<InputFrameAssembler>);

}  // namespace kfm::net
