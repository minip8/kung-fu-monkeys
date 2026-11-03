// Visitor-based serialization (architecture §5.2). One visit() per type
// yields read, write, checksum and (later) debug dump. Network snapshots,
// replays and checksums go through here, never through raw memory, because
// padding bytes are not deterministic.
//
// A visitor V provides:
//   void u8(uint8_t&); void u16(uint16_t&); void u32(uint32_t&);
//   void u64(uint64_t&); void i32(int32_t&); void boolean(bool&);
// Reading visitors overwrite the reference; others only read it.
#pragma once

#include "kfm/core/bitstream.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/hash.hpp"
#include "kfm/core/pool.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/types.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/tags.hpp"
#include "kfm/sim/world_state.hpp"

#include <cstdint>
#include <type_traits>

namespace kfm::sim {

// --- core types -------------------------------------------------------------

template <class V>
void visit(V& v, Tick& t) {
  v.u32(t.value);
}
template <class V>
void visit(V& v, PlayerId& p) {
  v.u8(p.value);
}
template <class V>
void visit(V& v, EntityId& e) {
  v.u16(e.index);
  v.u16(e.generation);
}
template <class V>
void visit(V& v, Fixed& f) {
  v.i32(f.raw);
}
template <class V>
void visit(V& v, Angle& a) {
  v.u16(a.bam);
}
template <class V>
void visit(V& v, Vec2& p) {
  visit(v, p.x);
  visit(v, p.y);
}
template <class V>
void visit(V& v, Rng& r) {
  for (uint32_t& word : r.s)
    v.u32(word);
}

// Number of valid values of each visited enum (values are 0..count-1). A
// reader rejects anything at or above it: a snapshot from the network must
// not be able to put an out-of-range enum into the state, where it would
// index a table or shift by it.
template <class E>
inline constexpr uint8_t kEnumCount = 0;  // no specialisation: not visitable
template <>
inline constexpr uint8_t kEnumCount<AbilityKind> = static_cast<uint8_t>(AbilityKind::kCount);
template <>
inline constexpr uint8_t kEnumCount<AbilityPhase> = 4;
template <>
inline constexpr uint8_t kEnumCount<StatKind> = 3;
template <>
inline constexpr uint8_t kEnumCount<BoomerangPhase> = 3;
template <>
inline constexpr uint8_t kEnumCount<PickupKind> = 1;
template <>
inline constexpr uint8_t kEnumCount<MatchPhase> = 5;
static_assert(static_cast<uint8_t>(AbilityPhase::Recovery) + 1 == kEnumCount<AbilityPhase>);
static_assert(static_cast<uint8_t>(StatKind::AmmoMax) + 1 == kEnumCount<StatKind>);
static_assert(static_cast<uint8_t>(BoomerangPhase::Grounded) + 1 == kEnumCount<BoomerangPhase>);
static_assert(static_cast<uint8_t>(PickupKind::Banana) + 1 == kEnumCount<PickupKind>);
static_assert(static_cast<uint8_t>(MatchPhase::MatchEnded) + 1 == kEnumCount<MatchPhase>);

template <class V, class E>
  requires std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, uint8_t>
void visit(V& v, E& e) {
  static_assert(kEnumCount<E> > 0, "add a kEnumCount specialisation for this enum");
  auto raw = static_cast<uint8_t>(e);
  v.u8(raw);
  if constexpr (V::kIsReader) {
    if (raw >= kEnumCount<E>) {
      v.markInvalid();
      raw = 0;
    }
    e = static_cast<E>(raw);
  }
}

// A reader rejects a player reference that is neither invalid nor a slot.
template <class V>
void validatePlayerRef(V& v, PlayerId& p) {
  if constexpr (V::kIsReader) {
    if (p.isValid() && p.value >= kMaxPlayers) {
      v.markInvalid();
      p = PlayerId::invalid();
    }
  }
}

// Pools visit the alive bits, the generations and the alive items only;
// dead slots are canonicalised away so two states that differ only in
// garbage left in dead slots compare equal.
template <class V, class T, uint16_t N>
void visit(V& v, Pool<T, N>& pool) {
  for (uint64_t& word : pool.aliveBits)
    v.u64(word);
  for (uint16_t& g : pool.generation)
    v.u16(g);
  v.u16(pool.aliveCount);
  if constexpr (V::kIsReader) {
    // A snapshot from the network must not be able to break pool invariants.
    if (!pool.invariantsHold()) {
      v.markInvalid();
      pool.clear();
      return;
    }
  }
  for (uint16_t i = 0; i < N; ++i) {
    if (pool.isAliveIndex(i)) {
      visit(v, pool.items[i]);
    } else if constexpr (V::kIsReader) {
      pool.items[i] = T{};
    }
  }
}

// --- sim types --------------------------------------------------------------

template <class V>
void visit(V& v, Tags& t) {
  v.u32(t.bits);
  if constexpr (V::kIsReader) {
    constexpr uint32_t kValid = (uint32_t{1} << static_cast<uint32_t>(Tag::kCount)) - 1u;
    if ((t.bits & ~kValid) != 0) {
      v.markInvalid();
      t.bits &= kValid;
    }
  }
}

// --- content (hashed for content_hash, architecture §5.10) ------------------

template <class V>
void visit(V& v, AbilityDef& d) {
  visit(v, d.kind);
  v.u16(d.cooldownTicks);
  v.u16(d.startupTicks);
  v.u16(d.activeTicks);
  v.u16(d.recoveryTicks);
  v.u16(d.perfectWindowTicks);
  visit(v, d.speed);
  visit(v, d.param0);
  visit(v, d.param1);
  visit(v, d.tagsStartup);
  visit(v, d.tagsActive);
  visit(v, d.tagsRecovery);
}
template <class V>
void visit(V& v, BoomerangDef& d) {
  visit(v, d.radius);
  visit(v, d.speed);
  v.u16(d.outboundTicks);
  v.u16(d.maxReturnTicks);
  v.u16(d.turnRateBam);
  visit(v, d.restitution);
  visit(v, d.spawnOffset);
  v.u8(d.damage);
  v.u8(d.pierce);
}
template <class V>
void visit(V& v, PickupDef& d) {
  visit(v, d.radius);
}
template <class V>
void visit(V& v, PlayerDef& d) {
  visit(v, d.radius);
  visit(v, d.moveSpeed);
  v.u8(d.maxHp);
  v.u8(d.ammoMax);
  v.u8(d.ammoStart);
  v.u16(d.hitStunTicks);
  for (uint8_t& a : d.abilities)
    v.u8(a);
}
template <class V>
void visit(V& v, ArenaDef& d) {
  visit(v, d.halfExtents);
  for (Vec2& p : d.spawnPoints)
    visit(v, p);
  v.u8(d.spawnPointCount);
}
template <class V>
void visit(V& v, ModeDef& d) {
  v.u8(d.roundsToWin);
  v.u16(d.roundEndTicks);
  v.u16(d.roundStartTicks);
}
template <class V>
void visit(V& v, ContentDB& c) {
  for (AbilityDef& d : c.abilities)
    visit(v, d);
  v.u8(c.abilityCount);
  visit(v, c.boomerang);
  visit(v, c.pickup);
  visit(v, c.player);
  visit(v, c.arena);
  visit(v, c.mode);
}

// --- input and state ---------------------------------------------------------

template <class V>
void visit(V& v, PlayerInput& in) {
  v.u8(in.moveDir);
  v.u8(in.moveMag);
  v.u8(in.aimDir);
  v.u8(in.buttons);
  v.u8(in.subTick);
}
template <class V>
void visit(V& v, InputFrame& f) {
  visit(v, f.tick);
  v.u16(f.present);
  for (uint16_t i = 0; i < kMaxPlayers; ++i) {
    if (((static_cast<uint32_t>(f.present) >> i) & 1u) != 0) {
      visit(v, f.inputs[i]);
    } else if constexpr (V::kIsReader) {
      f.inputs[i] = PlayerInput{};
    }
  }
}
template <class V>
void visit(V& v, MatchRules& r) {
  visit(v, r.phase);
  v.u16(r.present);
  v.u32(r.ticksInPhase);
  v.u8(r.round);
  for (uint8_t& score : r.scores)
    v.u8(score);
  visit(v, r.lastRoundWinner);
  validatePlayerRef(v, r.lastRoundWinner);
}
template <class V>
void visit(V& v, Transform& t) {
  visit(v, t.pos);
  visit(v, t.prevPos);
  visit(v, t.vel);
  visit(v, t.facing);
}
template <class V>
void visit(V& v, Stats& st) {
  visit(v, st.moveSpeed);
  v.u8(st.maxHp);
  v.u8(st.ammoMax);
}
template <class V>
void visit(V& v, StatModifier& m) {
  visit(v, m.stat);
  visit(v, m.mul);
  visit(v, m.add);
  v.u16(m.ticksLeft);
}
// Only the live prefix is visited; slots past count are canonicalised on
// read so garbage there never reaches a checksum or the wire.
template <class V>
void visit(V& v, ModifierList& m) {
  v.u8(m.count);
  if constexpr (V::kIsReader) {
    if (m.count > kMaxModifiers) {
      v.markInvalid();
      m = ModifierList{};
      return;
    }
  }
  for (uint8_t i = 0; i < kMaxModifiers; ++i) {
    if (i < m.count) {
      visit(v, m.items[i]);
    } else if constexpr (V::kIsReader) {
      m.items[i] = StatModifier{};
    }
  }
}
template <class V>
void visit(V& v, Intent& in) {
  visit(v, in.move);
  visit(v, in.aim);
  v.u8(in.pressed);
  v.u8(in.held);
  v.u8(in.subTick);
}
template <class V>
void visit(V& v, AbilityState& a) {
  v.u8(a.defIndex);
  if constexpr (V::kIsReader) {
    if (a.defIndex >= kMaxAbilityDefs) {
      v.markInvalid();
      a.defIndex = 0;
    }
  }
  visit(v, a.phase);
  v.u16(a.phaseTicks);
  v.u16(a.cooldownTicks);
  visit(v, a.activatedTick);
  v.u8(a.activatedSubTick);
  visit(v, a.dir);
}
template <class V>
void visit(V& v, Player& p) {
  visit(v, p.id);
  visit(v, p.transform);
  visit(v, p.intent);
  v.u8(p.prevButtons);
  visit(v, p.stats);
  visit(v, p.modifiers);
  v.u8(p.hp);
  v.u8(p.ammo);
  visit(v, p.tags);
  v.u16(p.hitStunTicks);
  for (AbilityState& a : p.abilities)
    visit(v, a);
}
template <class V>
void visit(V& v, Boomerang& b) {
  visit(v, b.owner);
  validatePlayerRef(v, b.owner);
  visit(v, b.phase);
  visit(v, b.transform);
  visit(v, b.spin);
  v.u16(b.ticksAlive);
  v.u16(b.ticksInPhase);
  v.u8(b.damage);
  v.u8(b.pierce);
}
template <class V>
void visit(V& v, Pickup& p) {
  visit(v, p.kind);
  visit(v, p.pos);
  v.u8(p.powerUpIndex);
}
template <class V>
void visit(V& v, WorldState& s) {
  visit(v, s.tick);
  visit(v, s.rng);
  visit(v, s.rules);
  visit(v, s.players);
  if constexpr (V::kIsReader) {
    // Players are indexed by slot (WorldState::playerBySlot): an alive
    // player's id must equal its pool index, or rules index by a stray id.
    for (uint16_t i = 0; i < kMaxPlayers; ++i) {
      Player& p = s.players.items[i];
      if (s.players.isAliveIndex(i) && p.id.value != i) {
        v.markInvalid();
        p.id = PlayerId{static_cast<PlayerId::rep>(i)};
      }
    }
  }
  visit(v, s.boomerangs);
  visit(v, s.pickups);
}

// --- visitors ---------------------------------------------------------------

class ChecksumVisitor {
 public:
  static constexpr bool kIsReader = false;
  void u8(uint8_t& x) { h_.add(static_cast<uint32_t>(x)); }
  void u16(uint16_t& x) { h_.add(static_cast<uint32_t>(x)); }
  void u32(uint32_t& x) { h_.add(x); }
  void u64(uint64_t& x) { h_.add(x); }
  void i32(int32_t& x) { h_.add(x); }
  void boolean(bool& x) { h_.add(x ? 1u : 0u); }
  void markInvalid() {}
  [[nodiscard]] uint32_t finish() const { return h_.finish(); }

 private:
  Hash32 h_;
};

class WriteVisitor {
 public:
  static constexpr bool kIsReader = false;
  explicit WriteVisitor(BitWriter& w) : w_(w) {}
  void u8(uint8_t& x) { w_.writeU8(x); }
  void u16(uint16_t& x) { w_.writeU16(x); }
  void u32(uint32_t& x) { w_.writeU32(x); }
  void u64(uint64_t& x) { w_.writeU64(x); }
  void i32(int32_t& x) { w_.writeI32(x); }
  void boolean(bool& x) { w_.writeBool(x); }
  void markInvalid() {}
  [[nodiscard]] bool ok() const { return w_.ok(); }

 private:
  BitWriter& w_;
};

class ReadVisitor {
 public:
  static constexpr bool kIsReader = true;
  explicit ReadVisitor(BitReader& r) : r_(r) {}
  void u8(uint8_t& x) { x = r_.readU8(); }
  void u16(uint16_t& x) { x = r_.readU16(); }
  void u32(uint32_t& x) { x = r_.readU32(); }
  void u64(uint64_t& x) { x = r_.readU64(); }
  void i32(int32_t& x) { x = r_.readI32(); }
  void boolean(bool& x) { x = r_.readBool(); }
  // Called by a visit() that found the decoded value structurally invalid.
  void markInvalid() { invalid_ = true; }
  [[nodiscard]] bool ok() const { return r_.ok() && !invalid_; }

 private:
  BitReader& r_;
  bool invalid_{false};
};

// Checksum of any visitable value. The visitor never writes, so the
// const_cast is safe; visit() takes non-const refs so one function serves
// reading too.
template <class T>
[[nodiscard]] uint32_t checksumOf(const T& value) {
  ChecksumVisitor v;
  visit(v, const_cast<T&>(value));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return v.finish();
}

template <class T>
bool writeTo(BitWriter& w, const T& value) {
  WriteVisitor v(w);
  visit(v, const_cast<T&>(value));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return v.ok();
}

template <class T>
bool readFrom(BitReader& r, T& value) {
  ReadVisitor v(r);
  visit(v, value);
  return v.ok();
}

}  // namespace kfm::sim
