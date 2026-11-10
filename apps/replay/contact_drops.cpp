// Contact-list overflow accounting (see contact_drops.hpp).
#include "contact_drops.hpp"

#include "kfm/sim/contacts.hpp"

#include <cstdio>
#include <format>

namespace kfm::replay {

void ContactDrops::add(Tick tick, uint16_t dropped, uint32_t match) {
  if (dropped == 0) return;
  total += dropped;
  ++ticks;
  if (dropped > worst) {
    worst = dropped;
    worstTick = tick;
    worstMatch = match;
  }
}

void ContactDrops::merge(const ContactDrops& other) {
  total += other.total;
  ticks += other.ticks;
  if (other.worst > worst) {
    worst = other.worst;
    worstTick = other.worstTick;
    worstMatch = other.worstMatch;
  }
}

std::string contactDropsLine(const ContactDrops& drops, bool perMatch, bool fatal) {
  if (!drops.any()) return {};
  const std::string where =
      perMatch ? std::format("match {} tick {}", drops.worstMatch, drops.worstTick.value)
               : std::format("tick {}", drops.worstTick.value);
  return std::format(
      "{}: contact list overflow (kMaxContacts {}): {} contacts dropped over {} ticks, worst {} "
      "with {}\n",
      fatal ? "error" : "warning", sim::kMaxContacts, drops.total, drops.ticks, where, drops.worst);
}

void reportContactDrops(const ContactDrops& drops, bool perMatch, bool fatal) {
  const std::string line = contactDropsLine(drops, perMatch, fatal);
  if (!line.empty()) std::fputs(line.c_str(), stderr);
}

}  // namespace kfm::replay
