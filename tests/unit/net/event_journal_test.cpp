#include "kfm/net/event_journal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

constexpr sim::SimEvent ev(sim::EventKind kind, uint8_t player, uint32_t a = 0) {
  sim::SimEvent e;
  e.kind = kind;
  e.player = PlayerId{player};
  e.a = a;
  return e;
}

// Simulates tick t with the given events.
void simulate(EventJournal& j, uint32_t t, std::initializer_list<sim::SimEvent> events) {
  j.beginTick(Tick{t});
  for (const sim::SimEvent& e : events)
    j.record(e);
  j.endTick();
}

std::vector<sim::SimEvent> drainAll(EventJournal& j) {
  std::array<sim::SimEvent, 64> buf{};
  const size_t n = j.drain(buf);
  return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

constexpr sim::SimEvent kDash = ev(sim::EventKind::PlayerDashed, 1);
constexpr sim::SimEvent kThrow = ev(sim::EventKind::BoomerangThrown, 2);
constexpr sim::SimEvent kHit = ev(sim::EventKind::PlayerHit, 3, 2);

}  // namespace

TEST_CASE("events re-emitted by a re-simulation are delivered once", "[net][journal]") {
  EventJournal j;
  simulate(j, 10, {kDash});
  simulate(j, 11, {kThrow});
  auto out = drainAll(j);
  REQUIRE(out.size() == 2);
  CHECK(out[0].tick == Tick{10});
  CHECK(out[1].tick == Tick{11});

  // Rollback to 10: both ticks re-emit the same events.
  simulate(j, 10, {kDash});
  simulate(j, 11, {kThrow});
  CHECK(drainAll(j).empty());
  CHECK(j.stats().deduplicated == 2);
  CHECK(j.stats().dropped == 0);
  CHECK(j.stats().retracted == 0);
}

TEST_CASE("undelivered events of an abandoned timeline are dropped", "[net][journal]") {
  EventJournal j;
  simulate(j, 5, {kDash, kThrow});
  // Before the host drained, a rollback re-simulates tick 5 without the throw
  // and with a hit instead.
  simulate(j, 5, {kDash, kHit});
  const auto out = drainAll(j);
  REQUIRE(out.size() == 2);
  CHECK(out[0].kind == sim::EventKind::PlayerDashed);
  CHECK(out[1].kind == sim::EventKind::PlayerHit);
  CHECK(j.stats().dropped == 1);
  CHECK(j.stats().deduplicated == 1);
}

TEST_CASE("delivered events of an abandoned timeline are retracted, not replayed",
          "[net][journal]") {
  EventJournal j;
  simulate(j, 5, {kThrow});
  CHECK(drainAll(j).size() == 1);
  simulate(j, 5, {});
  CHECK(drainAll(j).empty());
  CHECK(j.stats().retracted == 1);
  // If the throw comes back in a later re-simulation it is new again.
  simulate(j, 5, {kThrow});
  CHECK(drainAll(j).size() == 1);
}

TEST_CASE("events at or before the final tick survive a re-simulation", "[net][journal]") {
  EventJournal j;
  simulate(j, 3, {kHit});
  j.confirm(Tick{3});
  CHECK(j.finalTick() == Tick{3});
  simulate(j, 3, {});  // cannot happen in a client, but must not drop a final event
  const auto out = drainAll(j);
  REQUIRE(out.size() == 1);
  CHECK(out[0].kind == sim::EventKind::PlayerHit);
  CHECK(j.stats().dropped == 0);
  // Delivered and final: forgotten.
  CHECK(j.size() == 0);
}

TEST_CASE("equal events within a tick match by multiplicity", "[net][journal]") {
  EventJournal j;
  simulate(j, 8, {kDash, kDash});
  CHECK(drainAll(j).size() == 2);
  simulate(j, 8, {kDash, kDash, kDash});  // one more in the new timeline
  CHECK(drainAll(j).size() == 1);
  simulate(j, 8, {kDash});  // two fewer
  CHECK(j.stats().retracted == 2);
}

TEST_CASE("a re-simulation in the middle keeps the journal in tick order", "[net][journal]") {
  EventJournal j;
  simulate(j, 1, {kDash});
  simulate(j, 2, {});
  simulate(j, 3, {kThrow});
  // Rollback to 2: tick 2 now has a hit, tick 3 still the throw.
  simulate(j, 2, {kHit});
  simulate(j, 3, {kThrow});
  const auto out = drainAll(j);
  REQUIRE(out.size() == 3);
  CHECK(out[0].tick == Tick{1});
  CHECK(out[1].tick == Tick{2});
  CHECK(out[1].kind == sim::EventKind::PlayerHit);
  CHECK(out[2].tick == Tick{3});
}

TEST_CASE("the payload, not the tick, is hashed", "[net][journal]") {
  const sim::SimEvent a = kHit;
  sim::SimEvent b = kHit;
  b.tick = Tick{99};
  CHECK(EventJournal::hashOf(a) == EventJournal::hashOf(b));
  b.b = 1;
  CHECK(EventJournal::hashOf(a) != EventJournal::hashOf(b));
}

TEST_CASE("a full journal evicts delivered events first and counts overflow", "[net][journal]") {
  EventJournal j;
  uint32_t t = 0;
  for (size_t i = 0; i < EventJournal::kCapacity; ++i)
    simulate(j, t++, {kDash});
  CHECK(j.size() == EventJournal::kCapacity);
  simulate(j, t++, {kThrow});  // nothing delivered: no room
  CHECK(j.stats().overflow == 1);
  CHECK(drainAll(j).size() == 64);
  simulate(j, t++, {kThrow});  // evicts the oldest delivered one
  CHECK(j.stats().overflow == 1);
  CHECK(j.size() == EventJournal::kCapacity);
}
