// Bit-level writer and reader over a caller-provided byte span. Used for
// network messages, snapshots and replays (architecture §6.3).
//
// Layout: bits are packed least-significant-bit first into bytes in order;
// a value written with n bits occupies the next n bits. The layout is a
// wire-format contract and must not change without a protocol version bump.
//
// The writer clears each byte when it first touches it, so the buffer does
// not need to be zero-initialised and send buffers can be reused.
//
// Neither class throws or allocates. Running past the end sets an overflow
// flag; reads past the end yield zero. Callers check ok() at the end.
#pragma once

#include "kfm/core/assert.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace kfm {

class BitWriter {
 public:
  constexpr explicit BitWriter(std::span<std::byte> buffer) : buf_(buffer) {}

  // Writes the low `bits` bits of value, 1..32.
  constexpr void writeBits(uint32_t value, int bits) {
    KFM_ASSERT(bits >= 1 && bits <= 32);
    if (bits < 32) KFM_ASSERT((value >> bits) == 0);
    if (bitPos_ + static_cast<size_t>(bits) > buf_.size() * 8) {
      overflow_ = true;
      return;
    }
    while (bits > 0) {
      const size_t byteIndex = bitPos_ / 8;
      const int bitOffset = static_cast<int>(bitPos_ % 8);
      const int room = 8 - bitOffset;
      const int n = bits < room ? bits : room;
      const auto mask = static_cast<uint8_t>((1u << n) - 1u);
      const auto chunk = static_cast<uint8_t>((value & mask) << bitOffset);
      const uint8_t existing = bitOffset == 0 ? 0 : static_cast<uint8_t>(buf_[byteIndex]);
      buf_[byteIndex] = static_cast<std::byte>(existing | chunk);
      value >>= n;
      bits -= n;
      bitPos_ += static_cast<size_t>(n);
    }
  }

  constexpr void writeBool(bool b) { writeBits(b ? 1u : 0u, 1); }
  constexpr void writeU8(uint8_t v) { writeBits(v, 8); }
  constexpr void writeU16(uint16_t v) { writeBits(v, 16); }
  constexpr void writeU32(uint32_t v) { writeBits(v, 32); }
  constexpr void writeU64(uint64_t v) {
    writeBits(static_cast<uint32_t>(v), 32);
    writeBits(static_cast<uint32_t>(v >> 32), 32);
  }
  constexpr void writeI32(int32_t v) { writeBits(static_cast<uint32_t>(v), 32); }
  constexpr void writeI16(int16_t v) { writeBits(static_cast<uint16_t>(v), 16); }
  constexpr void writeI8(int8_t v) { writeBits(static_cast<uint8_t>(v), 8); }

  constexpr void writeBytes(std::span<const std::byte> bytes) {
    for (const std::byte b : bytes)
      writeU8(static_cast<uint8_t>(b));
  }

  // Pads with zero bits to the next byte boundary.
  constexpr void align() {
    const size_t rem = bitPos_ % 8;
    if (rem != 0) writeBits(0, static_cast<int>(8 - rem));
  }

  [[nodiscard]] constexpr size_t bitsWritten() const { return bitPos_; }
  [[nodiscard]] constexpr size_t bytesWritten() const { return (bitPos_ + 7) / 8; }
  [[nodiscard]] constexpr bool ok() const { return !overflow_; }
  [[nodiscard]] constexpr bool overflowed() const { return overflow_; }
  [[nodiscard]] constexpr std::span<const std::byte> written() const {
    return buf_.first(bytesWritten());
  }

 private:
  std::span<std::byte> buf_;
  size_t bitPos_{0};
  bool overflow_{false};
};

class BitReader {
 public:
  constexpr explicit BitReader(std::span<const std::byte> buffer) : buf_(buffer) {}

  // Reads `bits` bits, 1..32. Returns 0 and flags overflow past the end.
  [[nodiscard]] constexpr uint32_t readBits(int bits) {
    KFM_ASSERT(bits >= 1 && bits <= 32);
    if (bitPos_ + static_cast<size_t>(bits) > buf_.size() * 8) {
      overflow_ = true;
      return 0;
    }
    uint32_t value = 0;
    int shift = 0;
    while (bits > 0) {
      const size_t byteIndex = bitPos_ / 8;
      const int bitOffset = static_cast<int>(bitPos_ % 8);
      const int room = 8 - bitOffset;
      const int n = bits < room ? bits : room;
      const uint32_t mask = (1u << n) - 1u;
      const uint32_t chunk = (static_cast<uint32_t>(buf_[byteIndex]) >> bitOffset) & mask;
      value |= chunk << shift;
      shift += n;
      bits -= n;
      bitPos_ += static_cast<size_t>(n);
    }
    return value;
  }

  [[nodiscard]] constexpr bool readBool() { return readBits(1) != 0; }
  [[nodiscard]] constexpr uint8_t readU8() { return static_cast<uint8_t>(readBits(8)); }
  [[nodiscard]] constexpr uint16_t readU16() { return static_cast<uint16_t>(readBits(16)); }
  [[nodiscard]] constexpr uint32_t readU32() { return readBits(32); }
  [[nodiscard]] constexpr uint64_t readU64() {
    const uint64_t lo = readBits(32);
    const uint64_t hi = readBits(32);
    return lo | (hi << 32);
  }
  [[nodiscard]] constexpr int32_t readI32() { return static_cast<int32_t>(readBits(32)); }
  [[nodiscard]] constexpr int16_t readI16() { return static_cast<int16_t>(readBits(16)); }
  [[nodiscard]] constexpr int8_t readI8() { return static_cast<int8_t>(readBits(8)); }

  constexpr void readBytes(std::span<std::byte> out) {
    for (std::byte& b : out)
      b = static_cast<std::byte>(readU8());
  }

  constexpr void align() {
    const size_t rem = bitPos_ % 8;
    if (rem != 0) static_cast<void>(readBits(static_cast<int>(8 - rem)));
  }

  [[nodiscard]] constexpr size_t bitsRead() const { return bitPos_; }
  [[nodiscard]] constexpr size_t bitsRemaining() const { return buf_.size() * 8 - bitPos_; }
  [[nodiscard]] constexpr bool ok() const { return !overflow_; }
  [[nodiscard]] constexpr bool overflowed() const { return overflow_; }
  [[nodiscard]] constexpr bool atEnd() const { return bitsRemaining() == 0; }

 private:
  std::span<const std::byte> buf_;
  size_t bitPos_{0};
  bool overflow_{false};
};

}  // namespace kfm
