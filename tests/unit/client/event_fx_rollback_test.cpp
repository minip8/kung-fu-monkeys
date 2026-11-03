// Effects and sounds under rollback (architecture §5.9, §7): presentation
// consumes the EventJournal's output, the same stream NetSession hands the
// builder, so an event a rollback retracted before it was drained spawns no
// effect and no sound, and a re-simulated duplicate never plays twice.
#include "../sim/sim_test_util.hpp"
#include "kfm/net/event_journal.hpp"
#include "kfm/presentation/builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

using namespace kfm;
using namespace kfm::presentation;
using namespace kfm::sim;

namespace {

void recordTick(net::EventJournal& j, uint32_t t, std::span<const SimEvent> events) {
  j.beginTick(Tick{t});
  for (const SimEvent& e : events)
    j.record(e);
  j.endTick();
}

struct Drained {
  std::array<SimEvent, 64> buf{};
  std::size_t n{0};
  [[nodiscard]] std::span<const SimEvent> span() const { return {buf.data(), n}; }
};

Drained drain(net::EventJournal& j) {
  Drained d;
  d.n = j.drain(d.buf);
  return d;
}

}  // namespace

TEST_CASE("a rollback-retracted event spawns no effect and no sound", "[client][vfx][audio]") {
  WorldState s = test::makePlayingWorld(31, 0b11);
  test::placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  test::placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const ContentDB& content = defaultContent();
  PresentationState ps;
  RenderState rs;
  buildRenderState(ps, s, s, 1.0f, {}, content, 1.0f / 60.0f, rs);

  const std::array<SimEvent, 2> predicted = {
      SimEvent{EventKind::PlayerHit, Tick{5}, PlayerId{1}, EntityId{}, 0, 0},
      SimEvent{EventKind::PlayerEliminated, Tick{5}, PlayerId{1}, EntityId{}, 0, 0},
  };
  net::EventJournal j;
  recordTick(j, 5, predicted);
  // A rollback re-simulates tick 5 before the host drained: the server's
  // inputs say the banana missed.
  recordTick(j, 5, {});
  const Drained d = drain(j);
  CHECK(d.n == 0);
  CHECK(j.stats().dropped == 2);
  buildRenderState(ps, s, s, 1.0f, d.span(), content, 1.0f / 60.0f, rs);
  CHECK(rs.effectCount == 0);
  CHECK(rs.soundCount == 0);
  CHECK(rs.shakeAmplitude == 0.0f);
}

TEST_CASE("a re-simulated event plays its effect and sound once", "[client][vfx][audio]") {
  WorldState s = test::makePlayingWorld(32, 0b11);
  test::placePlayer(s, PlayerId{0}, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  const ContentDB& content = defaultContent();
  PresentationState ps;
  RenderState rs;
  buildRenderState(ps, s, s, 1.0f, {}, content, 1.0f / 60.0f, rs);

  const std::array<SimEvent, 1> dash = {
      SimEvent{EventKind::PlayerDashed, Tick{7}, PlayerId{0}, s.players.idAt(0), 0, 0}};
  net::EventJournal j;
  recordTick(j, 7, dash);
  Drained d = drain(j);
  REQUIRE(d.n == 1);
  buildRenderState(ps, s, s, 1.0f, d.span(), content, 1.0f / 60.0f, rs);
  const uint16_t effects = rs.effectCount;
  CHECK(effects > 0);
  REQUIRE(rs.soundCount == 1);
  CHECK(rs.sounds[0].id == SoundId::DashSwish);

  // Rollback: tick 7 is re-simulated and emits the same dash again.
  recordTick(j, 7, dash);
  d = drain(j);
  CHECK(d.n == 0);
  CHECK(j.stats().deduplicated == 1);
  buildRenderState(ps, s, s, 1.0f, d.span(), content, 1.0f / 60.0f, rs);
  CHECK(rs.effectCount <= effects);  // nothing new started
  CHECK(rs.soundCount == 0);
}
