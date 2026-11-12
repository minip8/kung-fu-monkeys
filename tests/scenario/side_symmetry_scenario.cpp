// Mirrored inputs give a mirrored world (architecture §11 "Contest design",
// v0.79: "a scenario test that mirrored inputs give mirrored outcomes comes
// first, because every gate above is read through it"). v0.80's contest row,
// control_1t since v0.81, on courtyard_zones, whose map is its own half turn
// (x, y) -> (-x, -y) with zone 1 and zone 2 exchanged and spawn point k of
// group 0 turning into point k of group 1 (tests/unit/content/contest_test.cpp),
// played through the real step() with every team-1 monkey given the half turn
// of the input its team-0 twin gets. Where the engine treats the two sides
// alike, the world stays its own half turn with the teams swapped on every
// tick; where it does not, the first tick and field that differ is the
// asymmetry. Since v0.81 the same for the contest row, harvest, on each
// arena of its pool (§11 "Contest design: bananas as the economy"), each its
// own half turn with zone 0 mapping to zone 1, under a gather-and-bank policy
// (the end of this file's "what holds").
//
// The slots: with ten present, teamsFor() deals them round-robin, so slot
// 2k is team 0's k-th monkey (the west, spawn group 0) and slot 2k + 1 team
// 1's k-th (the east, group 1); a role goes by the position in the team, so
// slot 0 and slot 1 are the catchers. The twin of slot i is slot i ^ 1,
// which keeps the order within each team (a same-team tie broken by the
// lower slot stays mirrored).
//
// The inputs are mirror-symmetric by construction: one Rng-free policy
// decides for a team-0 monkey from the world, and for a team-1 monkey from
// the world's mirror image (mirrorOf), with the answer turned back. So the
// inputs are each other's half turn whenever the world is its own, and the
// test reads only what the engine does with them. (Once fixed-point rounding
// has moved a banana by a few raw units, the policy reads it only through a
// cell-and-a-half threshold, so the inputs stay mirrored in practice; the
// matches below confirm it by staying mirrored.)
//
// Findings (v0.79's side-bias question): the engine is mirror-symmetric
// except for the known asymmetries pinned at the end of this file: two
// world-Rng draws that the two sides take separately (the spawn offsets, the
// spawners' power-ups), two slot-order tie-breaks that the lower slot (the
// west, team 0) wins (a simultaneous dash exchange, the order of
// simultaneous respawns), a cell contest or swap at an equal press, which
// since v0.81 alternates by the turn's parity (the west on even turns, the
// east on odd ones), and the floor rounding of Fixed's multiply, which
// nudges every banana toward (-x, -y).
#include "kfm/content/loader.hpp"
#include "kfm/sim/spaces/grid_cells.hpp"
#include "kfm/sim/zones.hpp"
#include "scenario_util.hpp"
#include "systems/angle_of.hpp"
#include "systems/statics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

const ContentDB& shipped() {
  static const ContentDB db = [] {
    ContentDB c{};
    content::Diagnostics diag;
    const bool loaded = content::loadEmbeddedContent(c, diag);
    REQUIRE(loaded);
    return c;
  }();
  return db;
}

[[nodiscard]] uint8_t modeNamed(std::string_view name) {
  const std::optional<uint8_t> i = shipped().findMode(name);
  INFO("mode " << name);
  REQUIRE(i.has_value());
  return i.value_or(0);
}

[[nodiscard]] uint8_t arenaNamed(std::string_view name) {
  const std::optional<uint8_t> i = shipped().findArena(name);
  INFO("arena " << name);
  REQUIRE(i.has_value());
  return i.value_or(0);
}

constexpr PlayerMask kTen = 0x3FF;

// --- the half turn --------------------------------------------------------------

[[nodiscard]] constexpr Vec2 turned(Vec2 v) {
  return Vec2{-v.x, -v.y};
}
[[nodiscard]] constexpr Angle turned(Angle a) {
  return a + Angle::fromBam(Angle::kHalf);
}
[[nodiscard]] constexpr PlayerInput turned(PlayerInput in) {
  in.moveDir = static_cast<uint8_t>(in.moveDir + 128u);
  in.aimDir = static_cast<uint8_t>(in.aimDir + 128u);
  return in;
}

// What the half turn does to a match's slots, teams, zones and spawners.
struct Mirror {
  std::array<uint8_t, kMaxPlayers> slot{};  // a slot's twin
  std::array<uint8_t, kMaxPlayers> team{};  // team 0 <-> 1, the rest stay
  std::array<uint8_t, kMaxArenaZones> zone{};
  std::array<uint8_t, kMaxPickupSpawners> spawner{};
  uint8_t zoneCount{0};
  uint8_t spawnerCount{0};
};

// The position of a slot within its team (slots in slot order): its role.
[[nodiscard]] uint8_t rankOf(const MatchContent& m, uint8_t slot) {
  uint8_t rank = 0;
  for (uint8_t j = 0; j < slot; ++j) {
    if (m.teams[j] == m.teams[slot]) ++rank;
  }
  return rank;
}

// The twins: team t's k-th slot and the other team's k-th. The arena's
// zones and spawners pair by turning their boxes and positions.
[[nodiscard]] Mirror mirrorFor(const MatchContent& m) {
  Mirror mi;
  for (uint8_t t = 0; t < kMaxPlayers; ++t) {
    mi.team[t] = t;
  }
  mi.team[0] = 1;
  mi.team[1] = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    mi.slot[i] = i;
    const uint8_t want = mi.team[m.teams[i]];
    const uint8_t rank = rankOf(m, i);
    for (uint8_t j = 0; j < kMaxPlayers; ++j) {
      if (m.teams[j] == want && rankOf(m, j) == rank) mi.slot[i] = j;
    }
  }
  const ArenaDef& a = m.arena;
  mi.zoneCount = std::min<uint8_t>(a.zoneCount, kMaxArenaZones);
  for (uint8_t k = 0; k < mi.zoneCount; ++k) {
    mi.zone[k] = 0xFF;
    for (uint8_t j = 0; j < mi.zoneCount; ++j) {
      if (turned(a.zones[k].max) == a.zones[j].min && turned(a.zones[k].min) == a.zones[j].max)
        mi.zone[k] = j;
    }
    INFO("zone " << int{k});
    REQUIRE(mi.zone[k] != 0xFF);
  }
  mi.spawnerCount = std::min<uint8_t>(a.pickupSpawnerCount, kMaxPickupSpawners);
  for (uint8_t k = 0; k < mi.spawnerCount; ++k) {
    mi.spawner[k] = 0xFF;
    for (uint8_t j = 0; j < mi.spawnerCount; ++j) {
      if (turned(a.pickupSpawners[k].pos) == a.pickupSpawners[j].pos) mi.spawner[k] = j;
    }
    INFO("spawner " << int{k});
    REQUIRE(mi.spawner[k] != 0xFF);
  }
  return mi;
}

[[nodiscard]] PlayerId twin(const Mirror& mi, PlayerId id) {
  return id.isValid() && id.value < kMaxPlayers ? PlayerId{mi.slot[id.value]} : id;
}

// modeData under the half turn: words 0-1 the clocks of teams 0-3 (u16
// each), swapped between the teams; for control, words 2-3 a nibble per zone
// (the team + 1, 0 nobody) and words 4-7 a u16 of progress per zone, the
// zones exchanged and the teams swapped. Other kinds keep words 2-7, and
// harvest every word.
[[nodiscard]] std::array<uint32_t, kModeDataWords> mirroredModeData(
    const std::array<uint32_t, kModeDataWords>& d, const Mirror& mi, ModeKind kind) {
  // Harvest (v0.81) keeps only its two beats in word 0, the same for both
  // sides.
  if (kind == ModeKind::Harvest) return d;
  std::array<uint32_t, kModeDataWords> out{};
  for (uint32_t t = 0; t < 4; ++t) {
    const uint32_t clock = (d[t / 2] >> ((t % 2) * 16)) & 0xFFFFu;
    const uint32_t to = mi.team[t];
    out[to / 2] |= clock << ((to % 2) * 16);
  }
  if (kind != ModeKind::Control) {
    for (uint8_t w = 2; w < kModeDataWords; ++w)
      out[w] = d[w];
    return out;
  }
  for (uint8_t k = 0; k < mi.zoneCount; ++k) {
    const uint8_t to = mi.zone[k];
    for (const uint8_t word : {uint8_t{2}, uint8_t{3}}) {
      const uint32_t v = (d[word] >> (4u * k)) & 0xFu;
      const uint32_t mapped = v == 0 ? 0u : uint32_t{mi.team[v - 1]} + 1u;
      out[word] |= mapped << (4u * to);
    }
    const uint32_t progress = (d[4 + (k / 2)] >> ((k % 2) * 16)) & 0xFFFFu;
    out[4 + (to / 2)] |= progress << ((to % 2) * 16);
  }
  return out;
}

[[nodiscard]] Player mirroredPlayer(const Player& p, const Mirror& mi) {
  Player q = p;
  q.id = twin(mi, p.id);
  q.transform.pos = turned(p.transform.pos);
  q.transform.prevPos = turned(p.transform.prevPos);
  q.transform.vel = turned(p.transform.vel);
  q.transform.facing = turned(p.transform.facing);
  q.intent.move = turned(p.intent.move);
  q.intent.aim = turned(p.intent.aim);
  for (AbilityState& st : q.abilities)
    st.dir = turned(st.dir);
  q.knockback = turned(p.knockback);
  q.moveTarget = turned(p.moveTarget);
  return q;
}

// The world's half turn with the teams swapped: what the world must equal,
// up to the order of the banana and pickup pools, for the engine to treat
// the two sides alike.
[[nodiscard]] WorldState mirrorOf(const WorldState& s, const Mirror& mi, ModeKind kind) {
  WorldState m = s;
  MatchRules& r = m.rules;
  r.present = 0;
  r.lastRoundWinner = twin(mi, s.rules.lastRoundWinner);
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const uint8_t j = mi.slot[i];
    if (maskHas(s.rules.present, PlayerId{i}))
      r.present = static_cast<PlayerMask>(r.present | (1u << j));
    r.scores[j] = s.rules.scores[i];
    r.deaths[j] = s.rules.deaths[i];
    r.respawnTicks[j] = s.rules.respawnTicks[i];
    r.invulnTicks[j] = s.rules.invulnTicks[i];
  }
  r.modeData = mirroredModeData(s.rules.modeData, mi, kind);
  m.players = {};
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const uint8_t j = mi.slot[i];
    m.players.items[j] =
        s.players.isAliveIndex(i) ? mirroredPlayer(s.players.items[i], mi) : s.players.items[i];
    m.players.generation[j] = s.players.generation[i];
    if (s.players.isAliveIndex(i)) {
      m.players.aliveBits[j / 64] |= uint64_t{1} << (j % 64);
      ++m.players.aliveCount;
    }
  }
  m.boomerangs.forEachAlive([&](EntityId /*id*/, Boomerang& b) {
    b.owner = twin(mi, b.owner);
    b.transform.pos = turned(b.transform.pos);
    b.transform.prevPos = turned(b.transform.prevPos);
    b.transform.vel = turned(b.transform.vel);
    b.transform.facing = turned(b.transform.facing);
  });
  m.pickups.forEachAlive([&](EntityId /*id*/, Pickup& k) {
    k.pos = turned(k.pos);
    if (k.kind == PickupKind::PowerUp && k.spawner < mi.spawnerCount)
      k.spawner = mi.spawner[k.spawner];
  });
  return m;
}

// --- comparing a world with its mirror ---------------------------------------------

[[nodiscard]] std::string str(Fixed f) {
  return std::to_string(f.raw);
}
[[nodiscard]] std::string str(Vec2 v) {
  return "(" + str(v.x) + ", " + str(v.y) + ")";
}
[[nodiscard]] std::string str(Angle a) {
  return std::to_string(a.bam) + " bam";
}
[[nodiscard]] std::string str(Tags t) {
  return std::to_string(t.bits);
}
[[nodiscard]] std::string str(Tick t) {
  return std::to_string(t.value);
}
[[nodiscard]] std::string str(PlayerId p) {
  return std::to_string(p.value);
}
template <class E>
  requires std::is_enum_v<E>
[[nodiscard]] std::string str(E e) {
  return std::to_string(static_cast<int>(e));
}
template <class I>
  requires std::is_integral_v<I>
[[nodiscard]] std::string str(I i) {
  return std::to_string(i);
}

// How far a continuous quantity may be off its mirror and still count as
// mirrored: 0 is bit for bit. The ulp-level slack lets a test read past the
// fixed-point rounding asymmetries (they are pinned on their own) to the
// outcomes: who hits, who catches, who scores.
struct Slack {
  int32_t ulps{0};  // Fixed and each Vec2 component, raw units
  int32_t bam{0};   // Angle

  [[nodiscard]] bool near(Fixed a, Fixed b) const {
    return std::abs(int64_t{a.raw} - b.raw) <= ulps;
  }
  [[nodiscard]] bool near(Vec2 a, Vec2 b) const { return near(a.x, b.x) && near(a.y, b.y); }
  [[nodiscard]] bool near(Angle a, Angle b) const {
    return std::abs(int32_t{a.signedDeltaFrom(b)}) <= bam;
  }
  template <class T>
  [[nodiscard]] bool near(const T& a, const T& b) const {
    return a == b;
  }
};

// The first field where a world and its mirror differ, as a sentence, or
// empty when they agree.
class Differ {
 public:
  explicit Differ(Slack slack) : slack_(slack) {}
  template <class T>
  void same(const std::string& what, const T& world, const T& mirror) {
    if (!found_.empty() || slack_.near(world, mirror)) return;
    found_ = what + ": " + str(world) + " vs the mirror's " + str(mirror);
  }
  void note(const std::string& what) {
    if (found_.empty()) found_ = what;
  }
  [[nodiscard]] bool clean() const { return found_.empty(); }
  [[nodiscard]] const std::string& found() const { return found_; }
  [[nodiscard]] const Slack& slack() const { return slack_; }

 private:
  Slack slack_;
  std::string found_;
};

void comparePlayer(Differ& d, const std::string& at, const Player& a, const Player& b) {
  d.same(at + " id", a.id, b.id);
  d.same(at + " pos", a.transform.pos, b.transform.pos);
  d.same(at + " prevPos", a.transform.prevPos, b.transform.prevPos);
  d.same(at + " vel", a.transform.vel, b.transform.vel);
  d.same(at + " facing", a.transform.facing, b.transform.facing);
  d.same(at + " intent.move", a.intent.move, b.intent.move);
  d.same(at + " intent.aim", a.intent.aim, b.intent.aim);
  d.same(at + " intent.pressed", a.intent.pressed, b.intent.pressed);
  d.same(at + " intent.held", a.intent.held, b.intent.held);
  d.same(at + " intent.subTick", a.intent.subTick, b.intent.subTick);
  d.same(at + " prevButtons", a.prevButtons, b.prevButtons);
  d.same(at + " stats.moveSpeed", a.stats.moveSpeed, b.stats.moveSpeed);
  d.same(at + " stats.maxHp", a.stats.maxHp, b.stats.maxHp);
  d.same(at + " stats.ammoMax", a.stats.ammoMax, b.stats.ammoMax);
  d.same(at + " stats.radius", a.stats.radius, b.stats.radius);
  d.same(at + " modifiers.count", a.modifiers.count, b.modifiers.count);
  for (uint8_t k = 0; k < kMaxModifiers; ++k) {
    d.same(at + " modifier " + str(k) + " powerUp", a.modifiers.items[k].powerUp,
           b.modifiers.items[k].powerUp);
    d.same(at + " modifier " + str(k) + " ticksLeft", a.modifiers.items[k].ticksLeft,
           b.modifiers.items[k].ticksLeft);
  }
  d.same(at + " hp", a.hp, b.hp);
  d.same(at + " ammo", a.ammo, b.ammo);
  d.same(at + " tags", a.tags, b.tags);
  d.same(at + " hitStunTicks", a.hitStunTicks, b.hitStunTicks);
  d.same(at + " grantedTags", a.grantedTags, b.grantedTags);
  for (uint8_t k = 0; k < kAbilitySlots; ++k) {
    const AbilityState& x = a.abilities[k];
    const AbilityState& y = b.abilities[k];
    const std::string slot = at + " ability " + str(k);
    d.same(slot + " defIndex", x.defIndex, y.defIndex);
    d.same(slot + " phase", x.phase, y.phase);
    d.same(slot + " phaseTicks", x.phaseTicks, y.phaseTicks);
    d.same(slot + " cooldownTicks", x.cooldownTicks, y.cooldownTicks);
    d.same(slot + " activatedTick", x.activatedTick, y.activatedTick);
    d.same(slot + " activatedSubTick", x.activatedSubTick, y.activatedSubTick);
    d.same(slot + " dir", x.dir, y.dir);
  }
  d.same(at + " knockback", a.knockback, b.knockback);
  d.same(at + " knockbackTicks", a.knockbackTicks, b.knockbackTicks);
  d.same(at + " moveTarget", a.moveTarget, b.moveTarget);
  d.same(at + " moveTicks", a.moveTicks, b.moveTicks);
  d.same(at + " fallTicks", a.fallTicks, b.fallTicks);
  d.same(at + " regrowTicks", a.regrowTicks, b.regrowTicks);
}

[[nodiscard]] bool sameBanana(const Slack& k, const Boomerang& a, const Boomerang& b) {
  return a.owner == b.owner && a.phase == b.phase && k.near(a.transform.pos, b.transform.pos) &&
         k.near(a.transform.prevPos, b.transform.prevPos) &&
         k.near(a.transform.vel, b.transform.vel) &&
         k.near(a.transform.facing, b.transform.facing) && a.spin == b.spin &&
         a.ticksAlive == b.ticksAlive && a.ticksInPhase == b.ticksInPhase && a.damage == b.damage &&
         a.speedMul == b.speedMul && a.radiusMul == b.radiusMul;
}

[[nodiscard]] std::string describe(const Boomerang& b) {
  return "banana of slot " + str(b.owner) + " phase " + str(b.phase) + " at " +
         str(b.transform.pos) + " vel " + str(b.transform.vel) + " facing " +
         str(b.transform.facing) + " age " + str(b.ticksAlive) + " prev " +
         str(b.transform.prevPos) + " spin " + str(b.spin) + " inPhase " + str(b.ticksInPhase) +
         " speedMul " + str(b.speedMul) + " radiusMul " + str(b.radiusMul);
}

[[nodiscard]] bool samePickup(const Slack& k, const Pickup& a, const Pickup& b) {
  return a.kind == b.kind && k.near(a.pos, b.pos) && a.powerUpIndex == b.powerUpIndex &&
         a.spawner == b.spawner && a.count == b.count;
}

[[nodiscard]] std::string describe(const Pickup& k) {
  return "pickup kind " + str(k.kind) + " at " + str(k.pos) + " power-up " + str(k.powerUpIndex) +
         " spawner " + str(k.spawner) + " count " + str(k.count);
}

// Each item of `world` matched to one equal item of `mirror`, whatever the
// pool order (the lower slot's banana takes the lower pool index).
template <class T, uint16_t N, class Same, class Describe>
void compareUnordered(Differ& d, const std::string& what, const Pool<T, N>& world,
                      const Pool<T, N>& mirror, Same same, Describe describeOf) {
  d.same(what + " count", world.size(), mirror.size());
  std::array<bool, N> used{};
  world.forEachAlive([&](EntityId /*id*/, const T& x) {
    if (!d.clean()) return;
    bool matched = false;
    mirror.forEachAlive([&](EntityId id, const T& y) {
      if (matched || used[id.index] || !same(d.slack(), x, y)) return;
      used[id.index] = true;
      matched = true;
    });
    if (matched) return;
    std::string closest;
    mirror.forEachAlive([&](EntityId id, const T& y) {
      if (!used[id.index]) closest += (closest.empty() ? "" : "; ") + describeOf(y);
    });
    d.note(what + ": the world's " + describeOf(x) + " has no twin in the mirror" +
           (closest.empty() ? std::string{} : ", whose unmatched are " + closest));
  });
}

// The first difference between the world and its mirror, or empty.
[[nodiscard]] std::string asymmetry(const WorldState& s, const Mirror& mi, ModeKind kind,
                                    Slack slack = {}, PlayerMask fresh = 0) {
  const WorldState m = mirrorOf(s, mi, kind);
  Differ d(slack);
  const MatchRules& a = s.rules;
  const MatchRules& b = m.rules;
  d.same("phase", a.phase, b.phase);
  d.same("present", a.present, b.present);
  d.same("ticksInPhase", a.ticksInPhase, b.ticksInPhase);
  d.same("round", a.round, b.round);
  d.same("lastRoundWinner", a.lastRoundWinner, b.lastRoundWinner);
  d.same("dryTicks", a.dryTicks, b.dryTicks);
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const std::string slot = "slot " + str(i);
    d.same(slot + " score", a.scores[i], b.scores[i]);
    d.same(slot + " deaths", a.deaths[i], b.deaths[i]);
    d.same(slot + " respawnTicks", a.respawnTicks[i], b.respawnTicks[i]);
    d.same(slot + " invulnTicks", a.invulnTicks[i], b.invulnTicks[i]);
  }
  for (uint8_t w = 0; w < kModeDataWords; ++w) {
    d.same("modeData word " + str(w), a.modeData[w], b.modeData[w]);
  }
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const std::string slot = "slot " + str(i);
    d.same(slot + " alive", s.players.isAliveIndex(i), m.players.isAliveIndex(i));
    d.same(slot + " generation", s.players.generation[i], m.players.generation[i]);
    if (!s.players.isAliveIndex(i) || !m.players.isAliveIndex(i)) continue;
    Player world = s.players.items[i];
    Player mirror = m.players.items[i];
    // A monkey (re)spawned this tick has an empty intent (aim 0 on both
    // sides) until the next InputSystem: nothing reads it in between.
    if (maskHas(fresh, PlayerId{i}) || maskHas(fresh, twin(mi, PlayerId{i}))) {
      world.intent = Intent{};
      mirror.intent = Intent{};
    }
    comparePlayer(d, slot, world, mirror);
  }
  compareUnordered(d, "bananas", s.boomerangs, m.boomerangs, sameBanana,
                   [](const Boomerang& x) { return describe(x); });
  compareUnordered(d, "pickups", s.pickups, m.pickups, samePickup,
                   [](const Pickup& x) { return describe(x); });
  return d.found();
}

// The events of one tick, mirrored: per kind, the same count and the twin
// slots. (Payloads are slots, zones, teams or indices, so only the kind and
// the player are compared; the world comparison covers the rest.)
[[nodiscard]] std::string eventAsymmetry(const EventSink& sink, const Mirror& mi) {
  std::vector<std::pair<EventKind, PlayerId>> world;
  std::vector<std::pair<EventKind, PlayerId>> mirror;
  for (const SimEvent& e : sink) {
    world.emplace_back(e.kind, e.player);
    mirror.emplace_back(e.kind, twin(mi, e.player));
  }
  const auto order = [](const std::pair<EventKind, PlayerId>& x,
                        const std::pair<EventKind, PlayerId>& y) {
    return x.first != y.first ? x.first < y.first : x.second.value < y.second.value;
  };
  std::ranges::sort(world, order);
  std::ranges::sort(mirror, order);
  for (size_t i = 0; i < world.size(); ++i) {
    if (world[i] != mirror[i]) {
      return "event kind " + str(world[i].first) + " for slot " + str(world[i].second) +
             " has no twin";
    }
  }
  return {};
}

// --- the policy ---------------------------------------------------------------

// What a script asks of the monkeys.
struct Script {
  bool zones{true};        // walk to the zones and hold them
  bool throws{true};       // throw at an enemy in range
  bool catches{true};      // dash into an enemy banana close by
  bool dashes{false};      // dash now and then
  bool pickups{true};      // the last of each team goes for power-ups
  bool noSwaps{false};     // never step into a cell an enemy stands in
  uint8_t throwRange{4};   // cells, Chebyshev
  uint8_t throwEvery{3};   // a throw only on ticks where (tick + rank) % throwEvery == 0
  uint8_t subTickStep{0};  // subTick = rank * subTickStep on a press
  bool harvest{false};     // v0.81: gather and bank instead (harvestDecide), nothing else
};

[[nodiscard]] Fixed chebyshev(Vec2 a, Vec2 b) {
  return max(abs(a.x - b.x), abs(a.y - b.y));
}

[[nodiscard]] bool inPlay(const Player& p) {
  return !p.tags.has(Tag::Eliminated) && !p.tags.has(Tag::Falling);
}

[[nodiscard]] uint8_t input8(Vec2 v) {
  return static_cast<uint8_t>(angleOf(v).bam >> 8);
}

// Lexicographic order on positions: a tie-break the mirror carries over
// (it is read in each side's own frame), unlike the pool order.
[[nodiscard]] bool before(Vec2 a, Vec2 b) {
  return a.x != b.x ? a.x < b.x : a.y < b.y;
}

// Steering on the cells: a breadth-first distance field from the goal over
// the cells a monkey may rest on (no wall, inside the box, no pit), and the
// neighbour step that
// goes down it (ties: the lower octant), diagonals only where both
// orthogonal cells are free, as the grid latch demands.
class Steering {
 public:
  Steering(const ArenaDef& a, Fixed radius) : a_(a) {
    const int32_t cols = grid::columns(a);
    const int32_t rows = grid::rows(a);
    REQUIRE(cols <= 64);
    REQUIRE(rows <= 64);
    for (int32_t r = 0; r < rows; ++r) {
      for (int32_t c = 0; c < cols; ++c) {
        const Vec2 centre = grid::cellCentre(a, grid::Cell{c, r});
        const Vec2 limit = a.halfExtents - Vec2{radius, radius};
        bool free = abs(centre.x) <= limit.x && abs(centre.y) <= limit.y &&
                    !statics::overlapsAnyWall(a, centre, radius);
        if (discOverPits(a, centre, radius)) free = false;
        free_[index(grid::Cell{c, r})] = free;
      }
    }
  }

  // The moveDir toward `goal` from `pos`, or nullopt to stay.
  [[nodiscard]] std::optional<uint8_t> toward(Vec2 pos, Vec2 goal) const {
    const grid::Cell from = grid::cellOf(a_, pos);
    const grid::Cell to = grid::cellOf(a_, goal);
    if (from == to || !grid::inside(a_, from) || !grid::inside(a_, to)) return std::nullopt;
    std::array<int16_t, kCells> dist{};
    dist.fill(-1);
    std::array<grid::Cell, kCells> queue{};
    size_t head = 0;
    size_t tail = 0;
    dist[index(to)] = 0;
    queue[tail++] = to;
    while (head < tail) {
      const grid::Cell k = queue[head++];
      for (uint8_t o = 0; o < grid::kOctants; ++o) {
        const grid::Cell n = grid::neighbour(k, o);
        if (!step(k, o) || dist[index(n)] >= 0) continue;
        dist[index(n)] = static_cast<int16_t>(dist[index(k)] + 1);
        queue[tail++] = n;
      }
    }
    std::optional<uint8_t> best;
    int16_t bestDist = dist[index(from)] >= 0 ? dist[index(from)] : INT16_MAX;
    for (uint8_t o = 0; o < grid::kOctants; ++o) {
      const grid::Cell n = grid::neighbour(from, o);
      if (!step(from, o)) continue;
      const int16_t d = dist[index(n)];
      if (d >= 0 && d < bestDist) {
        bestDist = d;
        best = static_cast<uint8_t>(o * 32u);
      }
    }
    return best;
  }

 private:
  static constexpr size_t kCells = size_t{64} * 64;
  [[nodiscard]] static size_t index(grid::Cell k) {
    return static_cast<size_t>(k.row) * 64u + static_cast<size_t>(k.col);
  }
  [[nodiscard]] bool freeCell(grid::Cell k) const { return grid::inside(a_, k) && free_[index(k)]; }
  [[nodiscard]] bool step(grid::Cell from, uint8_t o) const {
    const grid::Cell to = grid::neighbour(from, o);
    if (!freeCell(to)) return false;
    if (!grid::diagonal(o)) return true;
    return freeCell(grid::Cell{to.col, from.row}) && freeCell(grid::Cell{from.col, to.row});
  }

  const ArenaDef& a_;
  std::array<bool, kCells> free_{};
};

// v0.81, harvest: one team-0 monkey gathering and banking, no fight (no
// throw, no dash). In its home with a banana it stays (banking them all);
// else, with room in its hand, it heads for the nearest banana pickup
// (Chebyshev, ties by position); with a full hand, or nothing to pick up
// and a banana in it, it heads home; empty-handed with nothing to pick up,
// it waits on the nearest tree. It never steps into a cell an enemy stands
// in: a twin stepping into its twin's cell across the centre is the swap
// pinned among the known asymmetries.
[[nodiscard]] PlayerInput harvestDecide(const WorldState& s, const MatchContent& m, PlayerId me) {
  PlayerInput in{};
  const Player* self = s.playerBySlot(me);
  if (self == nullptr || !inPlay(*self)) return in;
  const Vec2 pos = self->transform.pos;
  const uint8_t team = m.teamOf(me);
  const Zone& home = m.arena.zones[homeZoneOf(m.arena, team)];
  const bool inHome = boxContains(home.min, home.max, pos);
  if (inHome && self->ammo > 0) return in;

  const auto nearest = [&](std::optional<Vec2>& best, Vec2 p) {
    const Fixed d = chebyshev(p, pos);
    if (!best || d < chebyshev(*best, pos) || (d == chebyshev(*best, pos) && before(p, *best)))
      best = p;
  };
  std::optional<Vec2> goal;
  if (self->ammo < self->stats.ammoMax) {
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) nearest(goal, k.pos);
    });
  }
  if (!goal && self->ammo > 0) goal = home.centre();
  if (!goal) {
    for (uint8_t i = 0; i < m.arena.pickupSpawnerCount; ++i) {
      if (m.arena.pickupSpawners[i].isTree()) nearest(goal, m.arena.pickupSpawners[i].pos);
    }
  }
  if (!goal) return in;
  const Steering steer(m.arena, self->stats.radius);
  if (const std::optional<uint8_t> dir = steer.toward(pos, *goal)) {
    const grid::Cell next = grid::neighbour(grid::cellOf(m.arena, pos), grid::octantOf(*dir));
    bool taken = false;
    s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
      if (m.teamOf(p.id) != team && inPlay(p) && grid::cellOf(m.arena, p.transform.pos) == next)
        taken = true;
    });
    if (!taken) {
      in.moveDir = *dir;
      in.moveMag = 255;
    }
  }
  in.aimDir = in.moveDir;
  return in;
}

// One team-0 monkey's input from the world as it stands. Rng-free and
// deterministic; ties go by the rank of a slot, the index of a zone or the
// order of positions, all of which the mirror carries over.
[[nodiscard]] PlayerInput decide(const WorldState& s, const MatchContent& m, PlayerId me,
                                 const Script& script) {
  if (script.harvest) return harvestDecide(s, m, me);
  PlayerInput in{};
  const Player* self = s.playerBySlot(me);
  if (self == nullptr || !inPlay(*self)) return in;
  const Vec2 pos = self->transform.pos;
  const uint8_t team = m.teamOf(me);
  const uint8_t rank = rankOf(m, me.value);
  const uint32_t tick = s.rules.ticksInPhase;

  // The nearest enemy in play (ties: the lower rank).
  const Player* enemy = nullptr;
  Fixed enemyDist = Fixed::max();
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (m.teamOf(p.id) == team || !inPlay(p)) return;
    const Fixed dist = chebyshev(p.transform.pos, pos);
    if (enemy == nullptr || dist < enemyDist ||
        (dist == enemyDist && rankOf(m, p.id.value) < rankOf(m, enemy->id.value))) {
      enemy = &p;
      enemyDist = dist;
    }
  });

  // The goal.
  std::optional<Vec2> goal;
  bool hold = false;  // standing in the goal zone: stay
  if (script.pickups && rank == 4) {
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind != PickupKind::PowerUp) return;
      const Fixed d = chebyshev(k.pos, pos);
      if (!goal || d < chebyshev(*goal, pos) ||
          (d == chebyshev(*goal, pos) && before(k.pos, *goal)))
        goal = k.pos;
    });
  }
  if (!goal && script.zones && m.arena.zoneCount > 0) {
    // Ranks 0-2: the nearest zone the team does not own (else the
    // centre); rank 3 the centre; rank 4 the own home.
    std::optional<uint8_t> want;
    if (rank <= 2) {
      Fixed best = Fixed::max();
      for (uint8_t k = 0; k < m.arena.zoneCount; ++k) {
        const uint32_t owner = (s.rules.modeData[2] >> (4u * k)) & 0xFu;
        if (owner == uint32_t{team} + 1u) continue;
        const Fixed dist = chebyshev(m.arena.zones[k].centre(), pos);
        if (dist < best) {
          best = dist;
          want = k;
        }
      }
      if (!want) want = 0;
    } else if (rank == 4) {
      want = m.arena.zoneCount > 1 ? 1 : 0;  // team 0's home is the west zone
    } else {
      want = 0;
    }
    if (want) {
      const Zone& z = m.arena.zones[*want];
      goal = z.centre();
      hold = boxContains(z.min, z.max, pos);
    }
  }

  if (goal && !hold) {
    const Steering steer(m.arena, self->stats.radius);
    if (const std::optional<uint8_t> dir = steer.toward(pos, *goal)) {
      const grid::Cell next = grid::neighbour(grid::cellOf(m.arena, pos), grid::octantOf(*dir));
      bool taken = false;
      s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
        if (m.teamOf(p.id) != team && grid::cellOf(m.arena, p.transform.pos) == next) taken = true;
      });
      if (!script.noSwaps || !taken) {
        in.moveDir = *dir;
        in.moveMag = 255;
      }
    }
  }
  in.aimDir = enemy != nullptr ? input8(enemy->transform.pos - pos) : in.moveDir;

  // Catch: an enemy banana in flight within a cell and a half.
  bool incoming = false;
  s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
    if (b.phase != BoomerangPhase::Outbound && b.phase != BoomerangPhase::Returning) return;
    if (m.teamOf(b.owner) == team) return;
    if (chebyshev(b.transform.pos, pos) <= Fixed::fromRatio(3, 2)) incoming = true;
  });
  if (script.catches && incoming) in.buttons |= buttonBit(Button::Dash);
  if (script.dashes && (tick + (rank * 7u)) % 37u == 0) in.buttons |= buttonBit(Button::Dash);
  if (script.throws && enemy != nullptr && self->ammo > 0 &&
      enemyDist <= Fixed::fromInt(script.throwRange) &&
      (tick + rank) % std::max<uint32_t>(script.throwEvery, 1) == 0) {
    in.buttons |= buttonBit(Button::Throw);
  }
  if (in.buttons != 0) in.subTick = static_cast<uint8_t>(rank * script.subTickStep);
  return in;
}

// --- running a mirrored match --------------------------------------------------------

struct Run {
  MatchConfig config;
  MatchContent content;
  Mirror mirror;
  WorldState world;
  EventLog log;  // every event so far
  Slack slack;   // what play() stops at; the exact first difference is kept too
  std::string firstAsymmetry;
  uint32_t firstAsymmetricTick{0};
  bool asymmetric{false};
  std::string firstInexact;  // the first bit that differs, when slack is set
  uint32_t firstInexactTick{0};
  bool inexact{false};
  PlayerMask fresh{0};  // the slots (re)spawned on the last tick

  Run(uint8_t arena, uint8_t mode, uint64_t seed, uint16_t powerUps = kAllPowerUps,
      const ContentDB& db = shipped())
      : config{.seed = seed,
               .players = kTen,
               .arena = arena,
               .mode = mode,
               .powerUps = powerUps,
               .teams = teamsFor(db.modeAt(mode), kTen)},
        content(matchContent(db, config)),
        mirror(mirrorFor(content)),
        world(initialWorld(config)) {}

  [[nodiscard]] ModeKind kind() const { return content.mode.kind; }

  // The frame for this tick: team 0 decides from the world, team 1 from
  // its mirror, turned back.
  [[nodiscard]] InputFrame frame(const Script& script) const {
    InputFrame f = frameFor(world);
    f.present = config.players;
    const WorldState view = mirrorOf(world, mirror, kind());
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (content.teamOf(PlayerId{i}) == 0) {
        f.inputs[i] = decide(world, content, PlayerId{i}, script);
      } else {
        f.inputs[i] = turned(decide(view, content, PlayerId{mirror.slot[i]}, script));
      }
    }
    return f;
  }

  // One tick; records the first asymmetry (world or events) and its tick.
  void tick(const InputFrame& f) {
    EventSink sink;
    InputFrame g = f;
    g.tick = world.tick;
    step(world, g, content, sink);
    REQUIRE(sink.dropped == 0);
    log.insert(log.end(), sink.begin(), sink.end());
    const uint32_t stepped = world.tick.value - 1;
    fresh = 0;
    for (const SimEvent& e : sink) {
      if ((e.kind == EventKind::PlayerSpawned || e.kind == EventKind::PlayerRespawned) &&
          e.player.isValid() && e.player.value < kMaxPlayers)
        fresh = static_cast<PlayerMask>(fresh | (1u << e.player.value));
    }
    if (!inexact) {
      std::string found = asymmetry(world, mirror, kind(), {}, fresh);
      if (found.empty()) found = eventAsymmetry(sink, mirror);
      if (!found.empty()) {
        inexact = true;
        firstInexact = found;
        firstInexactTick = stepped;
      }
    }
    if (asymmetric || !inexact) return;
    std::string found = asymmetry(world, mirror, kind(), slack, fresh);
    if (found.empty()) found = eventAsymmetry(sink, mirror);
    if (!found.empty()) {
      asymmetric = true;
      firstAsymmetry = found;
      firstAsymmetricTick = stepped;
    }
  }

  // Plays the script until the match ends or `ticks` have run.
  void play(const Script& script, uint32_t ticks) {
    for (uint32_t t = 0; t < ticks && world.rules.phase != MatchPhase::MatchEnded; ++t) {
      tick(frame(script));
      if (asymmetric) return;
    }
  }

  [[nodiscard]] int count(EventKind kind) const { return countEvents(log, kind); }

  // A frame from team 0's inputs by rank, the twins given their half turn.
  [[nodiscard]] InputFrame mirrored(const std::array<PlayerInput, 5>& byRank) const {
    InputFrame f = frameFor(world);
    f.present = config.players;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
      if (content.teamOf(PlayerId{i}) != 0) continue;
      const PlayerInput in = byRank[rankOf(content, i) % byRank.size()];
      f.inputs[i] = in;
      f.inputs[mirror.slot[i]] = turned(in);
    }
    return f;
  }

  // Idle (mirrored) ticks until play starts.
  void untilPlaying() {
    for (int i = 0; i < 1000 && world.rules.phase != MatchPhase::Playing; ++i)
      tick(mirrored({}));
    REQUIRE(world.rules.phase == MatchPhase::Playing);
  }

  // Puts a slot on a cell centre and its twin on the half turn of it.
  void place(PlayerId id, Vec2 pos) {
    placePlayer(world, id, pos).moveTicks = 0;
    placePlayer(world, twin(mirror, id), turned(pos)).moveTicks = 0;
  }

  [[nodiscard]] std::string asymmetryNow() const {
    return asymmetry(world, mirror, kind(), {}, fresh);
  }
};

[[nodiscard]] int perfectCatches(const EventLog& log) {
  int n = 0;
  for (const SimEvent& e : log) {
    if (e.kind == EventKind::BoomerangCaught && e.a == static_cast<uint32_t>(CatchHow::Perfect))
      ++n;
  }
  return n;
}

[[nodiscard]] std::vector<SimEvent> eventsOf(const EventLog& log, EventKind kind) {
  std::vector<SimEvent> out;
  for (const SimEvent& e : log) {
    if (e.kind == kind) out.push_back(e);
  }
  return out;
}

[[nodiscard]] uint16_t onlyPowerUp(std::string_view name) {
  const std::optional<uint8_t> i = shipped().findPowerUp(name);
  INFO("power-up " << name);
  REQUIRE(i.has_value());
  return static_cast<uint16_t>(1u << i.value_or(0));
}

// v0.80's contest: control_1t on courtyard_zones since v0.81.
[[nodiscard]] uint8_t contestArena() {
  return arenaNamed("courtyard_zones");
}
[[nodiscard]] uint8_t contestMode() {
  return modeNamed("control_1t");
}

// A seed whose start is mirrored (both spawn groups draw the same offset:
// a known asymmetry below).
constexpr uint64_t kMirroredStart = 3;

// The point (x, y) / 2 (cell centres are at half units).
[[nodiscard]] constexpr Vec2 halves(int32_t x, int32_t y) {
  return Vec2{Fixed::fromRatio(x, 2), Fixed::fromRatio(y, 2)};
}

}  // namespace

// --- what holds ------------------------------------------------------------------

TEST_CASE("side symmetry: the half turn pairs slot 2k with 2k + 1, zone 1 with 2, spawner 0 with 1",
          "[scenario][symmetry]") {
  const Run run(contestArena(), contestMode(), kMirroredStart);
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    INFO("slot " << int{i});
    CHECK(run.content.teamOf(PlayerId{i}) == i % 2);  // team 0 is the west (group 0)
    CHECK(run.mirror.slot[i] == (i ^ 1u));
    // Twins play the same role: the catcher is each team's first slot.
    CHECK(run.content.loadoutIndexFor(PlayerId{i}) ==
          run.content.loadoutIndexFor(PlayerId{run.mirror.slot[i]}));
  }
  REQUIRE(run.mirror.zoneCount == 3);
  CHECK(run.mirror.zone[0] == 0);
  CHECK(run.mirror.zone[1] == 2);
  CHECK(run.mirror.zone[2] == 1);
  REQUIRE(run.mirror.spawnerCount == 2);
  CHECK(run.mirror.spawner[0] == 1);
  CHECK(run.mirror.spawner[1] == 0);
  // The sine table is odd under a half turn at every input direction (a
  // moveDir or aimDir is a 1/256 turn, a table sample).
  for (uint32_t d = 0; d < 256; ++d) {
    const Angle a = Angle::fromInput8(static_cast<uint8_t>(d));
    CHECK(Vec2::fromAngle(turned(a)) == turned(Vec2::fromAngle(a)));
  }
}

TEST_CASE(
    "side symmetry: a contest without banana fights is its own half turn, bit for bit, "
    "to a drawn end",
    "[scenario][symmetry]") {
  // Walking on the grid (cell contests between allies included), captures,
  // the clocks, points to each team's lowest slot, power-up spawns and
  // pickups, and the end: with one power-up in the spawners' set (the two
  // spawners draw separately, a known asymmetry below) the whole match is
  // mirrored exactly and ends 10 to 10, a draw.
  const std::string_view powerUp = GENERATE(as<std::string_view>{}, "big_banana_1t",
                                            "triplebanana_1t", "quick_hands_1t", "shield_1t");
  INFO(powerUp);
  Run run(contestArena(), contestMode(), kMirroredStart, onlyPowerUp(powerUp));
  run.play(Script{.throws = false, .catches = false, .subTickStep = 16}, 4000);
  INFO("tick " << run.firstInexactTick << ": " << run.firstInexact);
  CHECK_FALSE(run.inexact);
  CHECK(run.world.rules.phase == MatchPhase::MatchEnded);
  CHECK_FALSE(run.world.rules.lastRoundWinner.isValid());
  CHECK(run.content.teamScore(run.world.rules.scores, 0) == run.content.mode.scoreToWin);
  CHECK(run.content.teamScore(run.world.rules.scores, 1) == run.content.mode.scoreToWin);
  CHECK(run.count(EventKind::ZoneCaptured) == 2);  // each team its home
  CHECK(run.count(EventKind::PickupSpawned) >= 4);
  CHECK(run.count(EventKind::PickupTaken) >= 4);
}

TEST_CASE("side symmetry: banana fights stay mirrored to the end, up to fixed-point rounding",
          "[scenario][symmetry]") {
  // Throws that hit, own bananas caught home, perfect catches by the
  // catchers, eliminations and respawns, bounces, pickups and captures, a
  // full match: every discrete outcome is the twin's, and every position,
  // velocity and facing is within 256 raw units (1/256 of a cell) and 64 BAM
  // of the twin's. The rounding itself is a known asymmetry (below), there
  // from the first throw on, so these matches are not mirrored bit for bit.
  // The script never steps into a cell an enemy stands in (a swap with the
  // twin is a known asymmetry below), and on these seeds no twins respawn
  // into an empty side (another). v0.81: since an equal-press cell contest
  // between allies alternates by the turn (mirrored, as the lower slot
  // was), these matches walk other paths: seed 3's needs the dashing
  // test's slack (a banana drifts 65 raw units by tick 376, 128 by 395),
  // seed 5 stays within 96 raw units and 4 BAM (seed 16 plays seed 3's
  // match); the other mirrored starts tried (4, 54, 55, 58) meet another
  // known asymmetry, twins respawning into an emptier side.
  const uint64_t seed = GENERATE(3ull, 5ull);
  const std::string_view powerUp =
      GENERATE(as<std::string_view>{}, "big_banana_1t", "triplebanana_1t", "shield_1t");
  INFO("seed " << seed << ", " << powerUp);
  Run run(contestArena(), contestMode(), seed, onlyPowerUp(powerUp));
  run.slack = Slack{.ulps = 256, .bam = 64};
  run.play(Script{.noSwaps = true}, 4000);
  INFO("tick " << run.firstAsymmetricTick << ": " << run.firstAsymmetry);
  CHECK_FALSE(run.asymmetric);
  CHECK(run.world.rules.phase == MatchPhase::MatchEnded);
  CHECK_FALSE(run.world.rules.lastRoundWinner.isValid());
  CHECK(run.count(EventKind::PlayerHit) >= 20);
  CHECK(run.count(EventKind::PlayerEliminated) >= 10);
  CHECK(run.count(EventKind::PlayerRespawned) >= 8);
  CHECK(perfectCatches(run.log) >= 2);
  CHECK(run.count(EventKind::BoomerangCaught) > perfectCatches(run.log));
  CHECK(run.count(EventKind::BoomerangBounced) > 0);
  CHECK(run.count(EventKind::PickupTaken) >= 2);
  CHECK(run.count(EventKind::ZoneCaptured) == 2);
  // Known: the first bit that differs is a banana's (the rounding below).
  CHECK(run.inexact);
  CHECK(run.firstInexact.starts_with("bananas:"));
}

TEST_CASE("side symmetry: dashing fights stay mirrored to the end, up to fixed-point rounding",
          "[scenario][symmetry]") {
  // The same with a dash every 37 turns per monkey, the presses staggered
  // by rank (the earlier press wins a contested cell). Dashes put bananas
  // past wall corners more often, where the bounce's normal magnifies the
  // rounding: the slack is 256 raw units and 64 BAM here (still 1/256 of a
  // cell and a third of a degree), and the outcomes stay the twin's. v0.81:
  // with ally contests alternating by the turn, seed 3's match now meets
  // the simultaneous dash exchange below (slot 0's hp at tick 356) and the
  // mirrored starts that run to the end are seeds 5, 55, 74 and 92 (one
  // match: 88 dashes, 24 eliminations).
  const uint64_t seed = GENERATE(5ull, 55ull);
  INFO("seed " << seed);
  Run run(contestArena(), contestMode(), seed, onlyPowerUp("big_banana_1t"));
  run.slack = Slack{.ulps = 256, .bam = 64};
  run.play(Script{.dashes = true, .noSwaps = true, .subTickStep = 16}, 4000);
  INFO("tick " << run.firstAsymmetricTick << ": " << run.firstAsymmetry);
  CHECK_FALSE(run.asymmetric);
  CHECK(run.world.rules.phase == MatchPhase::MatchEnded);
  CHECK_FALSE(run.world.rules.lastRoundWinner.isValid());
  CHECK(run.count(EventKind::PlayerDashed) >= 50);
  CHECK(run.count(EventKind::PlayerEliminated) >= 10);
}

TEST_CASE(
    "side symmetry: a harvest of gathering and banking is its own half turn, bit for bit, "
    "on every arena of the contest's pool",
    "[scenario][symmetry][harvest]") {
  // v0.81: the contest row (harvest) on each member of its pool, with the
  // gather-and-bank policy (harvestDecide) and one power-up in the
  // spawners' set (the two spawners draw separately, a known asymmetry
  // below). Trees bear on their beats with no Rng draw, monkeys walk to the
  // fruit and carry it home, and bank there on the global beat: the whole
  // match is mirrored exactly, every tick to its end.
  const std::string_view arena =
      GENERATE(as<std::string_view>{}, "grove", "orchards", "gorge", "burrows");
  INFO(arena);
  Run run(arenaNamed(arena), modeNamed("contest"), kMirroredStart, onlyPowerUp("shield_1t"));
  REQUIRE(run.kind() == ModeKind::Harvest);
  REQUIRE(run.mirror.zoneCount == 2);
  CHECK(run.mirror.zone[0] == 1);
  CHECK(run.mirror.zone[1] == 0);
  run.play(Script{.harvest = true}, 5000);
  INFO("tick " << run.firstInexactTick << ": " << run.firstInexact);
  CHECK_FALSE(run.inexact);
  CHECK(run.world.rules.phase == MatchPhase::MatchEnded);
  CHECK_FALSE(run.world.rules.lastRoundWinner.isValid());  // a mirror draws
  const int banked = countEvents(run.log, EventKind::ScoreChanged);
  INFO("score changes " << banked << ", team scores "
                        << run.content.teamScore(run.world.rules.scores, 0) << " and "
                        << run.content.teamScore(run.world.rules.scores, 1));
  CHECK(run.content.teamScore(run.world.rules.scores, 0) ==
        run.content.teamScore(run.world.rules.scores, 1));
  CHECK(run.content.teamScore(run.world.rules.scores, 0) >= 10);
  CHECK(run.count(EventKind::PickupSpawned) >= 20);
  CHECK(run.count(EventKind::PickupTaken) >= 20);
  CHECK(run.count(EventKind::BoomerangThrown) == 0);
  CHECK(run.count(EventKind::PlayerEliminated) == 0);
}

// --- known asymmetries ---------------------------------------------------------------
// Each pins what the engine does today where the two sides are not treated
// alike (v0.79's side-bias question). A fix changes these on purpose.

TEST_CASE("known asymmetry: the two spawn groups draw their offsets separately",
          "[scenario][symmetry][known-asymmetry]") {
  // RulesSystem's spawnByGroups (systems/rules.cpp) draws one world-Rng
  // offset per spawn group, group 0 first; team t's k-th present slot takes
  // point (k + offset[t]) % 5 of its group. The start is mirrored only when
  // the two draws agree (1 in 5); otherwise team 1's monkeys stand on the
  // twins of team 0's points rotated by the difference, so the catcher (rank
  // 0) of one side starts where a thrower of the other does. Unbiased in
  // expectation (the draws are independent), but a single game does not
  // start mirrored.
  int mirroredStarts = 0;
  for (uint64_t seed = 1; seed <= 50; ++seed) {
    Run run(contestArena(), contestMode(), seed);
    run.tick(run.mirrored({}));  // the match start spawns everyone
    REQUIRE(run.count(EventKind::PlayerSpawned) == 10);
    if (run.asymmetryNow().empty()) ++mirroredStarts;
    if (seed == 1) CHECK(run.asymmetryNow().starts_with("slot 0 pos:"));
    if (seed == kMirroredStart) CHECK(run.asymmetryNow().empty());
  }
  CHECK(mirroredStarts == 6);
}

TEST_CASE("known asymmetry: the two power-up spawners draw their kinds separately",
          "[scenario][symmetry][known-asymmetry]") {
  // LifecycleSystem's runSpawners (systems/lifecycle.cpp) draws each
  // spawner's power-up from the world Rng in spawner order; courtyard_zones'
  // two spawners are each other's half turn and beat together, so the
  // pickups are each other's twin only when the draws agree (1 in 4 with
  // the contest's four twins). Unbiased in expectation, not mirrored.
  int alike = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    Run run(contestArena(), contestMode(), seed);
    run.untilPlaying();
    while (run.world.rules.ticksInPhase <= 90)
      run.tick(run.mirrored({}));
    std::array<int, 2> kind{-1, -1};
    run.world.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::PowerUp && k.spawner < 2) kind[k.spawner] = k.powerUpIndex;
    });
    REQUIRE(kind[0] >= 0);
    REQUIRE(kind[1] >= 0);
    if (kind[0] == kind[1]) ++alike;
  }
  CHECK(alike == 12);
}

TEST_CASE("known asymmetry: a throw's velocity rounds toward minus infinity on both axes",
          "[scenario][symmetry][known-asymmetry]") {
  // Fixed's operator* shifts the 64-bit product right, which floors, so
  // x * s and (-x) * s differ by one raw unit whenever x * s has a
  // fraction. The banana's velocity (abilities/throw.cpp, st.dir * speed),
  // the return's steering (ProjectileSystem's Vec2::fromAngle(facing,
  // speed), between table samples too) and the bounce response
  // (systems/statics.hpp) all multiply, so a banana's motion is biased by
  // up to a raw unit per axis per tick toward (-x, -y) whichever side threw
  // it: an east-side banana thrown west goes a little farther, a west-side
  // banana thrown east a little shorter. At 2 cells a tick that is 1/131072
  // of the flight; the full matches above stay within 64 raw units.
  const Fixed speed = Fixed::fromRaw(131070);  // the contest's 2 cells a tick
  const Fixed c = Fixed::fromRaw(46341);       // cos 45 degrees, a table sample
  CHECK((c * speed).raw == 92680);
  CHECK(((-c) * speed).raw == -92681);

  Run run(contestArena(), contestMode(), kMirroredStart);
  run.untilPlaying();
  run.place(PlayerId{2}, halves(-9, -1));  // west of the centre, row 7; its twin east, row 6
  REQUIRE(run.asymmetryNow().empty());
  std::array<PlayerInput, 5> in{};
  in[1] = PlayerInput{.aimDir = 96, .buttons = buttonBit(Button::Throw)};  // north-west
  run.tick(run.mirrored(in));
  run.tick(run.mirrored({}));
  std::optional<Vec2> west;
  std::optional<Vec2> east;
  run.world.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& b) {
    if (b.owner == PlayerId{2}) west = b.transform.vel;
    if (b.owner == PlayerId{3}) east = b.transform.vel;
  });
  REQUIRE(west.has_value());
  REQUIRE(east.has_value());
  CHECK(west.value_or(Vec2{}) == Vec2{Fixed::fromRaw(-92681), Fixed::fromRaw(92680)});
  CHECK(east.value_or(Vec2{}) == Vec2{Fixed::fromRaw(92680), Fixed::fromRaw(-92681)});
  CHECK(turned(east.value_or(Vec2{})) != west.value_or(Vec2{}));
  CHECK(run.asymmetryNow().starts_with("bananas:"));
  // Two ticks of flight: a raw unit per axis per tick, and the heading
  // (angleOf the velocity) a BAM apart.
  CHECK(asymmetry(run.world, run.mirror, run.kind(), Slack{.ulps = 2, .bam = 1}).empty());
  CHECK_FALSE(asymmetry(run.world, run.mirror, run.kind(), Slack{.ulps = 1, .bam = 1}).empty());
}

TEST_CASE("known asymmetry: twins swapping cells, the turn's slot steps and the other stays",
          "[scenario][symmetry][known-asymmetry]") {
  // The grid latch (spaces/grid.cpp, beats()) settles a contested cell or a
  // swap by the earlier press (the frame's subTick), then (v0.81) by the
  // turn: the lower slot on an even turn (ticksInPhase / turn_ticks), the
  // higher on an odd one. Twins press alike, so between them one side wins
  // each turn and the world leaves its half turn for that contest, but the
  // win alternates: team 0's slot (the west) on even turns, its twin (the
  // east) on odd ones, Rng-free and even over a match. Here slot 2 and its
  // twin slot 3 stand on the two centre cells that are each other's half
  // turn and step into each other's. (Before v0.81 the lower slot won every
  // time: 973 of 1585 enemy contests went west in 200 ref:hard games.)
  const std::array<PlayerInput, 5> in = [] {
    std::array<PlayerInput, 5> a{};
    a[1] = PlayerInput{.moveDir = 96, .moveMag = 255, .aimDir = 96};  // north-west, into the twin
    return a;
  }();
  const auto contest = [&](Run& run, bool odd) {
    run.untilPlaying();
    // The contest row's turn is one tick: a mirrored idle tick changes the
    // parity.
    if ((run.world.rules.ticksInPhase % 2 != 0) != odd) run.tick(run.mirrored({}));
    REQUIRE((run.world.rules.ticksInPhase % 2 != 0) == odd);
    run.place(PlayerId{2}, halves(1, -1));  // column 12 row 7; slot 3 on column 11 row 6
    REQUIRE(run.asymmetryNow().empty());
  };
  SECTION("an even turn: the lower slot (west) steps, its twin stays") {
    Run run(contestArena(), contestMode(), kMirroredStart);
    contest(run, false);
    run.tick(run.mirrored(in));
    CHECK(playerAt(run.world, PlayerId{2}).transform.pos == halves(-1, 1));  // stepped
    CHECK(playerAt(run.world, PlayerId{3}).transform.pos == halves(-1, 1));  // stayed
    CHECK(run.asymmetryNow().starts_with("slot 2 pos:"));
  }
  SECTION("an odd turn: the higher slot (east) steps, its twin stays") {
    Run run(contestArena(), contestMode(), kMirroredStart);
    contest(run, true);
    run.tick(run.mirrored(in));
    CHECK(playerAt(run.world, PlayerId{2}).transform.pos == halves(1, -1));  // stayed
    CHECK(playerAt(run.world, PlayerId{3}).transform.pos == halves(1, -1));  // stepped
    CHECK_FALSE(run.asymmetryNow().empty());
  }
  SECTION("the earlier press wins whichever slot it is and whatever the turn") {
    for (const bool odd : {false, true}) {
      INFO("odd turn " << odd);
      Run early(contestArena(), contestMode(), kMirroredStart);
      contest(early, odd);
      InputFrame f = early.mirrored(in);
      f.inputs[2].subTick = 10;  // the twin presses first
      early.tick(f);
      CHECK(playerAt(early.world, PlayerId{2}).transform.pos == halves(1, -1));  // stayed
      CHECK(playerAt(early.world, PlayerId{3}).transform.pos == halves(1, -1));  // stepped
    }
  }
}

TEST_CASE("known asymmetry: twins dashing into each other, the lower slot strikes first",
          "[scenario][symmetry][known-asymmetry]") {
  // InteractionSystem (systems/interaction.cpp) orders contacts by time of
  // impact, then the dash press (activatedTick, activatedSubTick), then the
  // pool index of A. Twins dash on the same tick and subTick and meet at
  // the same time, so the lower slot's dash attack resolves first: its twin
  // takes the hit and its HitStun bars the return blow (row 25 forbids
  // HitStun on the attacker). Team 0 (the west) wins every such exchange
  // with its twin; outside a mirror, any exactly simultaneous exchange goes
  // to the lower slot (15 of the 25 cross-team rank pairs to the west).
  Run run(contestArena(), contestMode(), kMirroredStart);
  run.untilPlaying();
  run.place(PlayerId{2}, halves(1, -1));
  REQUIRE(playerAt(run.world, PlayerId{2}).ammo >= shipped().dashAttack.minAmmo);
  REQUIRE(run.asymmetryNow().empty());
  std::array<PlayerInput, 5> in{};
  in[1] = PlayerInput{.aimDir = 96, .buttons = buttonBit(Button::Dash)};  // north-west, at the twin
  run.tick(run.mirrored(in));
  const std::vector<SimEvent> hits = eventsOf(run.log, EventKind::PlayerDashHit);
  REQUIRE(hits.size() == 1);
  CHECK(hits[0].player == PlayerId{3});  // the victim
  CHECK(hits[0].a == 2u);                // the attacker
  CHECK(playerAt(run.world, PlayerId{3}).hp + 1 == playerAt(run.world, PlayerId{2}).hp);
  CHECK(run.asymmetryNow().starts_with("slot 2 hp:"));
}

TEST_CASE("known asymmetry: twins respawning on one tick, the lower slot first",
          "[scenario][symmetry][known-asymmetry]") {
  // LifecycleSystem respawns the due slots in slot order, and respawnPlayer
  // (modes/mode_flow.cpp, farthestSpawnPoint) takes the point of the
  // monkey's group farthest from the nearest monkey in play, ties drawn from
  // the world Rng. Twins die on the same tick, so they respawn on the same
  // tick: the lower slot (team 0) chooses without its twin on the board,
  // the twin with it, and each draws its own tie-break. With other monkeys
  // standing the choice is theirs and stays mirrored; into an empty arena
  // (all ten down, every point tied) it does not.
  SECTION("among standing monkeys the respawn is mirrored") {
    Run run(contestArena(), contestMode(), kMirroredStart);
    run.untilPlaying();
    for (const uint8_t slot : {uint8_t{4}, uint8_t{5}}) {
      run.world.players.despawn(run.world.players.idAt(slot));
      run.world.rules.respawnTicks[slot] = 1;
    }
    REQUIRE(run.asymmetryNow().empty());
    run.tick(run.mirrored({}));
    REQUIRE(run.count(EventKind::PlayerRespawned) == 2);
    CHECK(run.asymmetryNow().empty());
  }
  SECTION("into an empty arena it is not") {
    Run run(contestArena(), contestMode(), kMirroredStart);
    run.untilPlaying();
    for (uint8_t slot = 0; slot < kMaxPlayers; ++slot) {
      run.world.players.despawn(run.world.players.idAt(slot));
      run.world.rules.respawnTicks[slot] = 1;
    }
    REQUIRE(run.asymmetryNow().empty());
    run.tick(run.mirrored({}));
    const std::vector<SimEvent> spawns = eventsOf(run.log, EventKind::PlayerRespawned);
    REQUIRE(spawns.size() == 10);
    // Spawn point k of group 0 is point k + 5 of group 1 (its twin).
    CHECK(spawns[0].player == PlayerId{0});
    CHECK(spawns[1].player == PlayerId{1});
    CHECK(spawns[1].a != spawns[0].a + 5);
    CHECK(run.asymmetryNow().starts_with("slot 0 pos:"));
  }
}
