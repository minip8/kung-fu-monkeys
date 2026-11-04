// The scripted bots use the third slot (architecture §9, v0.21): with blink,
// slam or parry in a loadout, a scripted match produces that ability's
// events (the corpus entries scripted_4p_{blink,slam,parry}_1800t rely on
// it), and with the slot empty the bots never press the ability button.
#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "loadout_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

struct Tally {
  std::map<EventKind, int> events;
  int abilityPresses{0};
};

Tally runBots(const MatchContent& content, int ticks = 1800) {
  WorldState s = initialWorld(MatchConfig{0x4B464D61, 0b1111});
  Rng inputs = Rng::seeded(0x5C1261);
  Tally t;
  EventSink sink;
  for (int k = 0; k < ticks; ++k) {
    const InputFrame f = scriptedFrame(inputs, s, content);
    for (const PlayerInput& in : f.inputs) {
      if (in.has(Button::Ability1)) ++t.abilityPresses;
    }
    sink.clear();
    step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    for (const SimEvent& e : sink)
      ++t.events[e.kind];
  }
  return t;
}

// The default content plus a loadout that puts `def` on the ability button,
// selected through MatchConfig::loadout (v0.31).
struct LoadoutWith {
  ContentDB db;
  uint8_t loadout{0};
  explicit LoadoutWith(const AbilityDef& def) {
    uint8_t index = 0;
    db = withAbility(def, index);
    loadout = addLoadout(db, def.name.view(), index);
  }
  [[nodiscard]] MatchContent content() const { return withLoadoutMatch(db, loadout); }
};

}  // namespace

TEST_CASE("scripted bots blink, slam and parry when the loadout gives them the ability",
          "[scenario][bots]") {
  SECTION("blink") {
    const LoadoutWith m(blinkDef());
    Tally t = runBots(m.content());
    CHECK(t.events[EventKind::PlayerBlinked] > 0);
  }
  SECTION("slam") {
    const LoadoutWith m(slamDef());
    Tally t = runBots(m.content());
    CHECK(t.events[EventKind::SlamLanded] > 0);
    CHECK(t.events[EventKind::PlayerSlammed] > 0);
  }
  SECTION("parry") {
    const LoadoutWith m(parryDef());
    Tally t = runBots(m.content());
    CHECK(t.events[EventKind::PlayerParryStarted] > 0);
    CHECK(t.events[EventKind::BoomerangParried] > 0);
  }
  SECTION("the base kit: the ability button is never pressed") {
    Tally t = runBots(defaultContent());
    CHECK(t.abilityPresses == 0);
    CHECK(t.events[EventKind::PlayerBlinked] == 0);
  }
}
