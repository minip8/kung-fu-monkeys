#include "kfm/sim/visit.hpp"

#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>

using namespace kfm;
using namespace kfm::literals;
using namespace kfm::sim;

namespace {
WorldState sampleWorld() {
  WorldState s = initialWorld(MatchConfig{42, 0b0000001011});
  EventSink sink;
  for (int i = 0; i < 5; ++i) {
    InputFrame f;
    f.tick = s.tick;
    f.present = s.rules.present;
    f.inputs[0].moveDir = 64;
    step(s, f, sink);
  }
  return s;
}
}  // namespace

TEST_CASE("checksum is a function of visited state only", "[sim][visit]") {
  const WorldState a = sampleWorld();
  WorldState b = a;
  CHECK(checksum(a) == checksum(b));

  // Garbage in a dead pool slot must not change the checksum.
  b.players.items[5].transform.pos.x = 123_fx;
  CHECK(checksum(a) == checksum(b));

  // A change in an alive slot must.
  b = a;
  b.players.items[0].transform.pos.x += Fixed::epsilon();
  CHECK(checksum(a) != checksum(b));

  b = a;
  ++b.tick;
  CHECK(checksum(a) != checksum(b));
}

TEST_CASE("write/read visitor round-trips WorldState", "[sim][visit]") {
  const WorldState a = sampleWorld();
  std::array<std::byte, 8192> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));

  WorldState b{};
  b.players.items[7].id = PlayerId{7};  // garbage the reader must canonicalise away
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(r.atEnd());

  CHECK(b.tick == a.tick);
  CHECK(b.rng == a.rng);
  CHECK(b.players.size() == a.players.size());
  CHECK(checksum(a) == checksum(b));
  CHECK(b.players.items[7].id == PlayerId::invalid());
  a.players.forEachAlive([&](EntityId id, const Player& p) {
    const Player* q = b.players.get(id);
    REQUIRE(q != nullptr);
    CHECK(q->id == p.id);
    CHECK(q->transform.pos == p.transform.pos);
    CHECK(q->transform.facing == p.transform.facing);
  });
}

TEST_CASE("InputFrame round-trip only carries present players", "[sim][visit]") {
  InputFrame f;
  f.tick = Tick{77};
  f.present = 0b101;
  f.inputs[0] = PlayerInput{1, 2, 3, 4, 5};
  f.inputs[1] = PlayerInput{9, 9, 9, 9, 9};  // absent: not encoded
  f.inputs[2] = PlayerInput{6, 7, 8, 9, 10};
  std::array<std::byte, 64> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, f));
  CHECK(w.bytesWritten() == 4 + 2 + 2 * 5);
  InputFrame g;
  BitReader r(w.written());
  REQUIRE(readFrom(r, g));
  CHECK(g.tick == f.tick);
  CHECK(g.present == f.present);
  CHECK(g.inputs[0] == f.inputs[0]);
  CHECK(g.inputs[1] == PlayerInput{});
  CHECK(g.inputs[2] == f.inputs[2]);
}
