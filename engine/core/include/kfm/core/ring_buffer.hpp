// Fixed-capacity ring buffer. Pushing onto a full buffer overwrites the
// oldest element. Trivially copyable when T is.
#pragma once

#include "kfm/core/assert.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace kfm {

template <class T, uint32_t N>
  requires std::is_trivially_copyable_v<T> && (N > 0)
struct RingBuffer {
  std::array<T, N> items{};
  uint32_t head{0};  // index of the oldest element
  uint32_t count{0};

  static constexpr uint32_t kCapacity = N;

  [[nodiscard]] constexpr uint32_t size() const { return count; }
  [[nodiscard]] static constexpr uint32_t capacity() { return N; }
  [[nodiscard]] constexpr bool empty() const { return count == 0; }
  [[nodiscard]] constexpr bool full() const { return count == N; }

  constexpr void clear() {
    head = 0;
    count = 0;
  }

  constexpr void push(const T& v) {
    items[(head + count) % N] = v;
    if (count == N) {
      head = (head + 1) % N;
    } else {
      ++count;
    }
  }

  // Removes and returns the oldest element. Buffer must not be empty.
  constexpr T popFront() {
    KFM_ASSERT(count > 0);
    const T v = items[head];
    head = (head + 1) % N;
    --count;
    return v;
  }

  // i == 0 is the oldest element, size()-1 the newest.
  [[nodiscard]] constexpr T& operator[](uint32_t i) {
    KFM_ASSERT(i < count);
    return items[(head + i) % N];
  }
  [[nodiscard]] constexpr const T& operator[](uint32_t i) const {
    KFM_ASSERT(i < count);
    return items[(head + i) % N];
  }
  [[nodiscard]] constexpr T& front() { return (*this)[0]; }
  [[nodiscard]] constexpr const T& front() const { return (*this)[0]; }
  [[nodiscard]] constexpr T& back() { return (*this)[count - 1]; }
  [[nodiscard]] constexpr const T& back() const { return (*this)[count - 1]; }
};

}  // namespace kfm
