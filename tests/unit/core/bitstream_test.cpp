#include "kfm/core/bitstream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using kfm::BitReader;
using kfm::BitWriter;

TEST_CASE("BitWriter/BitReader round-trip mixed widths", "[core][bitstream]") {
  std::array<std::byte, 64> buf{};
  BitWriter w(buf);
  w.writeBool(true);
  w.writeBits(0b101, 3);
  w.writeU8(0xAB);
  w.writeBits(0x1FFFF, 17);
  w.writeU16(0xBEEF);
  w.writeI32(-123456789);
  w.writeU64(0x0123456789ABCDEFULL);
  w.writeI16(-2);
  w.writeI8(-128);
  w.writeBits(0xFFFFFFFFu, 32);
  w.align();
  REQUIRE(w.ok());
  CHECK(w.bitsWritten() % 8 == 0);

  BitReader r(w.written());
  CHECK(r.readBool() == true);
  CHECK(r.readBits(3) == 0b101);
  CHECK(r.readU8() == 0xAB);
  CHECK(r.readBits(17) == 0x1FFFF);
  CHECK(r.readU16() == 0xBEEF);
  CHECK(r.readI32() == -123456789);
  CHECK(r.readU64() == 0x0123456789ABCDEFULL);
  CHECK(r.readI16() == -2);
  CHECK(r.readI8() == -128);
  CHECK(r.readBits(32) == 0xFFFFFFFFu);
  r.align();
  CHECK(r.ok());
  CHECK(r.atEnd());
}

TEST_CASE("Bit layout is LSB-first and byte-ordered (wire contract)", "[core][bitstream]") {
  std::array<std::byte, 4> buf{};
  BitWriter w(buf);
  w.writeBits(0b1, 1);
  w.writeBits(0b11, 2);
  w.writeBits(0b1010, 4);
  w.writeBits(0x1F, 5);
  CHECK(w.bytesWritten() == 2);
  // bits: 1 | 11 | 1010 | 11111 → byte0 = 1 + (0b11<<1) + (0b1010<<3) + (0b1<<7) = 0b11010111
  CHECK(static_cast<unsigned>(buf[0]) == 0b11010111u);
  CHECK(static_cast<unsigned>(buf[1]) == 0b00001111u);
  w.writeU16(0x1234);
  CHECK(w.bytesWritten() == 4);
  BitReader r(buf);
  static_cast<void>(r.readBits(12));
  CHECK(r.readU16() == 0x1234);
}

TEST_CASE("Overflow is flagged, never UB", "[core][bitstream]") {
  std::array<std::byte, 2> buf{};
  BitWriter w(buf);
  w.writeU8(1);
  w.writeU8(2);
  CHECK(w.ok());
  w.writeBits(1, 1);
  CHECK(w.overflowed());
  CHECK(w.bytesWritten() == 2);

  BitReader r(buf);
  CHECK(r.readU16() == 0x0201);
  CHECK(r.ok());
  CHECK(r.readU8() == 0);
  CHECK(r.overflowed());
}

TEST_CASE("Bytes round-trip", "[core][bitstream]") {
  std::array<std::byte, 8> buf{};
  const std::array<std::byte, 3> in{std::byte{1}, std::byte{2}, std::byte{3}};
  BitWriter w(buf);
  w.writeBits(1, 3);
  w.writeBytes(in);
  BitReader r(w.written());
  CHECK(r.readBits(3) == 1);
  std::array<std::byte, 3> out{};
  r.readBytes(out);
  CHECK(out == in);
}
