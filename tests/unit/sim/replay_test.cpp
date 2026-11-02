#include "kfm/sim/replay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace kfm;
using namespace kfm::sim;

TEST_CASE("Replay header round-trip and validation", "[sim][replay]") {
  ReplayHeader h;
  h.seed = 0x1122334455667788ULL;
  h.contentHash = 0xC0FFEE;
  h.buildHash = 0xB01D;
  h.players = 0x3FF;
  h.tickCount = 600;

  std::vector<std::byte> buf(replayMaxBytes(h));
  BitWriter w(buf);
  writeReplayHeader(w, h);
  CHECK(w.bytesWritten() == kReplayHeaderBytes);

  BitReader r(w.written());
  ReplayHeader g;
  REQUIRE(readReplayHeader(r, g));
  CHECK(g.seed == h.seed);
  CHECK(g.contentHash == h.contentHash);
  CHECK(g.buildHash == h.buildHash);
  CHECK(g.players == h.players);
  CHECK(g.tickCount == h.tickCount);
  CHECK(g.matchConfig().seed == h.seed);
  CHECK(g.matchConfig().players == h.players);

  buf[0] = std::byte{'X'};
  BitReader bad(w.written());
  CHECK_FALSE(readReplayHeader(bad, g));

  const std::vector<std::byte> empty;
  BitReader none(empty);
  CHECK_FALSE(readReplayHeader(none, g));
}

TEST_CASE("Replay frames round-trip", "[sim][replay]") {
  ReplayHeader h;
  h.players = 0b11;
  h.tickCount = 3;
  std::vector<std::byte> buf(replayMaxBytes(h));
  BitWriter w(buf);
  writeReplayHeader(w, h);
  for (uint32_t t = 0; t < h.tickCount; ++t) {
    InputFrame f;
    f.tick = Tick{t};
    f.present = h.players;
    f.inputs[1].buttons = static_cast<uint8_t>(t);
    writeReplayFrame(w, f);
  }
  REQUIRE(w.ok());

  BitReader r(w.written());
  ReplayHeader g;
  REQUIRE(readReplayHeader(r, g));
  for (uint32_t t = 0; t < g.tickCount; ++t) {
    InputFrame f;
    REQUIRE(readReplayFrame(r, f));
    CHECK(f.tick == Tick{t});
    CHECK(f.present == h.players);
    CHECK(f.inputs[1].buttons == t);
  }
  CHECK(r.atEnd());
  InputFrame extra;
  CHECK_FALSE(readReplayFrame(r, extra));
}
