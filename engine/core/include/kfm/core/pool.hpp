// Fixed-capacity entity pool with generational handles (architecture §5.2).
//
// Trivially copyable so WorldState snapshots are a memcpy. Slots are never
// compacted: indices are stable and iteration order is index order, which
// is what keeps the simulation deterministic. Serialization goes through
// the visitor, never raw bytes.
#pragma once

#include "kfm/core/assert.hpp"
#include "kfm/core/types.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>

namespace kfm {

template <class T, uint16_t N>
  requires std::is_trivially_copyable_v<T> && (N > 0) && (N < EntityId::kInvalidIndex)
struct Pool {
  using value_type = T;
  static constexpr uint16_t kCapacity = N;
  static constexpr uint32_t kWords = (N + 63) / 64;

  std::array<T, N> items{};
  std::array<uint16_t, N> generation{};
  std::array<uint64_t, kWords> aliveBits{};
  uint16_t aliveCount{0};

  [[nodiscard]] constexpr uint16_t size() const { return aliveCount; }
  [[nodiscard]] static constexpr uint16_t capacity() { return N; }
  [[nodiscard]] constexpr bool empty() const { return aliveCount == 0; }
  [[nodiscard]] constexpr bool full() const { return aliveCount == N; }

  [[nodiscard]] constexpr bool isAliveIndex(uint16_t index) const {
    return index < N && ((aliveBits[index / 64] >> (index % 64)) & 1u) != 0;
  }
  [[nodiscard]] constexpr bool isAlive(EntityId id) const {
    return isAliveIndex(id.index) && generation[id.index] == id.generation;
  }

  // Lowest free slot, value-initialised. Returns an invalid id when full.
  constexpr EntityId spawn() {
    for (uint32_t w = 0; w < kWords; ++w) {
      const uint64_t freeBits = ~aliveBits[w];
      if (freeBits == 0) continue;
      const uint32_t index = w * 64 + static_cast<uint32_t>(std::countr_zero(freeBits));
      if (index >= N) break;
      aliveBits[w] |= uint64_t{1} << (index % 64);
      items[index] = T{};
      ++aliveCount;
      return EntityId{static_cast<uint16_t>(index), generation[index]};
    }
    return EntityId::invalid();
  }

  // Occupies a specific slot (used where slot index carries meaning, e.g.
  // player slots). Returns an invalid id when the slot is taken or out of range.
  constexpr EntityId spawnAt(uint16_t index) {
    if (index >= N || isAliveIndex(index)) return EntityId::invalid();
    aliveBits[index / 64] |= uint64_t{1} << (index % 64);
    items[index] = T{};
    ++aliveCount;
    return EntityId{index, generation[index]};
  }

  // Frees the slot and bumps its generation so stale handles are detected.
  // Despawning a stale or invalid handle is a no-op.
  constexpr void despawn(EntityId id) {
    if (!isAlive(id)) return;
    aliveBits[id.index / 64] &= ~(uint64_t{1} << (id.index % 64));
    ++generation[id.index];
    --aliveCount;
  }

  constexpr void clear() {
    for (uint16_t i = 0; i < N; ++i) {
      if (isAliveIndex(i)) ++generation[i];
    }
    aliveBits.fill(0);
    aliveCount = 0;
  }

  [[nodiscard]] constexpr T* get(EntityId id) { return isAlive(id) ? &items[id.index] : nullptr; }
  [[nodiscard]] constexpr const T* get(EntityId id) const {
    return isAlive(id) ? &items[id.index] : nullptr;
  }

  // Handle for a slot known to be alive (e.g. from forEachAlive).
  [[nodiscard]] constexpr EntityId idAt(uint16_t index) const {
    KFM_ASSERT(isAliveIndex(index));
    return EntityId{index, generation[index]};
  }

  // Visits alive slots in index order. f(EntityId, T&) or f(EntityId, const T&).
  // f may despawn the current entity; it must not spawn (iteration order).
  template <class F>
  constexpr void forEachAlive(F&& f) {
    for (uint32_t w = 0; w < kWords; ++w) {
      uint64_t bits = aliveBits[w];
      while (bits != 0) {
        const uint32_t index = w * 64 + static_cast<uint32_t>(std::countr_zero(bits));
        bits &= bits - 1;
        const auto i = static_cast<uint16_t>(index);
        f(EntityId{i, generation[i]}, items[i]);
      }
    }
  }
  template <class F>
  constexpr void forEachAlive(F&& f) const {
    for (uint32_t w = 0; w < kWords; ++w) {
      uint64_t bits = aliveBits[w];
      while (bits != 0) {
        const uint32_t index = w * 64 + static_cast<uint32_t>(std::countr_zero(bits));
        bits &= bits - 1;
        const auto i = static_cast<uint16_t>(index);
        f(EntityId{i, generation[i]}, items[i]);
      }
    }
  }
};

}  // namespace kfm
