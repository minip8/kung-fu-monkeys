// kfm_replay's contact-drop accounting (apps/replay/contact_drops.hpp, issue
// #70): the total, the ticks that dropped any, the worst tick (the earliest
// on a tie, also across merged matches) and the report line, which is empty
// when nothing was dropped.
#include "contact_drops.hpp"

#include "kfm/core/types.hpp"
#include "kfm/sim/contacts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using kfm::Tick;
using kfm::replay::ContactDrops;
using kfm::replay::contactDropsLine;

namespace {

// The capacity the line names (kMaxContacts).
std::string cap() {
  return std::to_string(kfm::sim::kMaxContacts);
}

}  // namespace

TEST_CASE("no drops: nothing counted and no line", "[replay][contact_drops]") {
  ContactDrops d;
  d.add(Tick{0}, 0);
  d.add(Tick{1}, 0, 3);
  CHECK_FALSE(d.any());
  CHECK(d.total == 0);
  CHECK(d.ticks == 0);
  CHECK(d.worst == 0);
  CHECK(contactDropsLine(d, false).empty());
  CHECK(contactDropsLine(d, true, true).empty());
  ContactDrops merged;
  merged.merge(d);
  CHECK_FALSE(merged.any());
}

TEST_CASE("total, ticks with drops and the earliest worst tick", "[replay][contact_drops]") {
  ContactDrops d;
  d.add(Tick{10}, 3);
  d.add(Tick{11}, 0);
  d.add(Tick{12}, 7);
  d.add(Tick{13}, 7);  // a tie keeps tick 12
  d.add(Tick{14}, 1);
  CHECK(d.any());
  CHECK(d.total == 18);
  CHECK(d.ticks == 4);
  CHECK(d.worst == 7);
  CHECK(d.worstTick == Tick{12});
  CHECK(d.worstMatch == 0);
  CHECK(contactDropsLine(d, false) == "warning: contact list overflow (kMaxContacts " + cap() +
                                          "): 18 contacts dropped over 4 ticks, "
                                          "worst tick 12 with 7\n");
  CHECK(contactDropsLine(d, false, true) == "error: contact list overflow (kMaxContacts " + cap() +
                                                "): 18 contacts dropped over 4 ticks, "
                                                "worst tick 12 with 7\n");
}

TEST_CASE("merging matches sums and keeps the earlier match on a tie", "[replay][contact_drops]") {
  ContactDrops m0;
  m0.add(Tick{5}, 2, 0);
  ContactDrops m1;
  m1.add(Tick{40}, 9, 1);
  m1.add(Tick{41}, 1, 1);
  ContactDrops m2;
  m2.add(Tick{3}, 9, 2);  // ties match 1's worst: match 1 stays

  ContactDrops total;
  total.merge(m0);
  total.merge(m1);
  total.merge(m2);
  CHECK(total.total == 21);
  CHECK(total.ticks == 4);
  CHECK(total.worst == 9);
  CHECK(total.worstTick == Tick{40});
  CHECK(total.worstMatch == 1);
  CHECK(contactDropsLine(total, true) == "warning: contact list overflow (kMaxContacts " + cap() +
                                             "): 21 contacts dropped over 4 ticks, "
                                             "worst match 1 tick 40 with 9\n");
}

TEST_CASE("the largest per-tick count a step can report sums without wrapping",
          "[replay][contact_drops]") {
  ContactDrops d;
  constexpr uint16_t kMax = 0xFFFF;
  for (uint32_t t = 0; t < 4; ++t)
    d.add(Tick{t}, kMax);
  CHECK(d.total == uint64_t{4} * kMax);
  CHECK(d.ticks == 4);
  CHECK(d.worst == kMax);
  CHECK(d.worstTick == Tick{0});
}
