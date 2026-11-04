// Feel-tuning statistics (see stats.hpp).
#include "stats.hpp"

#include "kfm/sim/constants.hpp"
#include "kfm/sim/systems.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace kfm::replay {
namespace {

using namespace kfm::sim;

constexpr double kTickSeconds = 1.0 / static_cast<double>(kTickHz);

[[nodiscard]] uint32_t keyOf(EntityId id) {
  return (uint32_t{id.index} << 16) | id.generation;
}

[[nodiscard]] double seconds(Tick from, Tick to) {
  return static_cast<double>(to.minus(from)) * kTickSeconds;
}

template <class T>
void append(std::vector<T>& to, const std::vector<T>& from) {
  to.insert(to.end(), from.begin(), from.end());
}

void mergeWait(Stats::ThrowerWait& to, const Stats::ThrowerWait& from) {
  append(to.regainSeconds, from.regainSeconds);
  to.eliminatedFirst += from.eliminatedFirst;
  to.roundEndedFirst += from.roundEndedFirst;
}

// InteractionSystem's resolution order (systems/interaction.cpp, §5.6,
// §5.11): every contact, plus each Player x Player contact seen from B; time
// of impact, then the earlier dash press of A when A holds the tag its rows
// key on (PerfectDash against a banana or pickup, Dashing against a player),
// then pool indices, then list position and orientation; keys are taken from
// the state before any contact resolves. Returns the oriented contacts.
[[nodiscard]] std::vector<Contact> interactionOrder(const WorldState& s, const Contacts& contacts) {
  using Key = std::tuple<Fixed, uint64_t, uint8_t, uint16_t, uint8_t, uint16_t, uint16_t, bool>;
  std::vector<std::pair<Key, Contact>> keyed;
  keyed.reserve(contacts.count);
  const auto add = [&](const Contact& c, uint16_t index, bool reversed) {
    uint64_t press = std::numeric_limits<uint64_t>::max();
    if (c.kindA == EntityKind::Player) {
      const Player* p = s.players.get(c.a);
      const Tag keyedTag = c.kindB == EntityKind::Player ? Tag::Dashing : Tag::PerfectDash;
      if (p != nullptr && p->tags.has(keyedTag)) {
        const AbilityState& dash = p->abilities[kAbilitySlotDash];
        press = (static_cast<uint64_t>(dash.activatedTick.value) << 8) | dash.activatedSubTick;
      }
    }
    keyed.emplace_back(Key{c.tHit, press, static_cast<uint8_t>(c.kindA), c.a.index,
                           static_cast<uint8_t>(c.kindB), c.b.index, index, reversed},
                       c);
  };
  for (uint16_t i = 0; i < contacts.count; ++i) {
    const Contact& c = contacts.items[i];
    add(c, i, false);
    if (c.kindA == EntityKind::Player && c.kindB == EntityKind::Player) {
      add(reversedContact(c), i, true);
    }
  }
  std::ranges::sort(keyed, {}, &std::pair<Key, Contact>::first);
  std::vector<Contact> order;
  order.reserve(keyed.size());
  for (const auto& k : keyed)
    order.push_back(k.second);
  return order;
}

[[nodiscard]] bool entityAlive(const WorldState& s, EntityKind kind, EntityId id) {
  switch (kind) {
    case EntityKind::Player:
      return s.players.isAlive(id);
    case EntityKind::Boomerang:
      return s.boomerangs.isAlive(id);
    case EntityKind::Pickup:
      return s.pickups.isAlive(id);
    case EntityKind::Hazard:
      return true;  // arena statics never despawn
  }
  return false;
}

// The dash press as a fractional tick (§5.11).
[[nodiscard]] double pressTime(const AbilityState& dash) {
  return static_cast<double>(dash.activatedTick.value) +
         static_cast<double>(dash.activatedSubTick) / 256.0;
}

struct Summary {
  size_t n{0};
  double mean{0};
  double p10{0};
  double p50{0};
  double p90{0};
};

[[nodiscard]] Summary summarise(std::vector<double> v) {
  Summary s;
  s.n = v.size();
  if (v.empty()) return s;
  std::ranges::sort(v);
  double sum = 0;
  for (const double x : v)
    sum += x;
  s.mean = sum / static_cast<double>(v.size());
  const auto at = [&](double q) {
    const auto i = static_cast<size_t>(std::lround(q * static_cast<double>(v.size() - 1)));
    return v[std::min(i, v.size() - 1)];
  };
  s.p10 = at(0.1);
  s.p50 = at(0.5);
  s.p90 = at(0.9);
  return s;
}

void printDistribution(const char* label, const std::vector<double>& v, const char* unit) {
  const Summary s = summarise(v);
  if (s.n == 0) {
    std::printf("  %-34s n 0\n", label);
    return;
  }
  std::printf("  %-34s n %-6zu mean %6.2f  p10 %6.2f  median %6.2f  p90 %6.2f %s\n", label, s.n,
              s.mean, s.p10, s.p50, s.p90, unit);
}

[[nodiscard]] double ratio(uint64_t a, uint64_t b) {
  return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
}

// One-tick bins of tHit - pressTime from -1 to kBins - 1, then an overflow bin.
void printDeltaHistogram(const char* label, const std::vector<double>& v, uint16_t window) {
  constexpr int kBins = 16;
  std::array<uint64_t, kBins + 2> bins{};  // [0] < -1 ... [kBins + 1] >= kBins - 1
  for (const double d : v) {
    const int b = static_cast<int>(std::floor(d)) + 2;
    bins[static_cast<size_t>(std::clamp(b, 0, kBins + 1))]++;
  }
  std::printf("  %s (tHit - press, ticks; perfect window [0, %u)):\n", label, window);
  const double total = static_cast<double>(std::max<size_t>(v.size(), 1));
  for (int b = 0; b < kBins + 2; ++b) {
    const uint64_t n = bins[static_cast<size_t>(b)];
    if (n == 0) continue;
    const int lo = b - 2;
    char range[32];
    if (b == 0) {
      std::snprintf(range, sizeof range, "< -1");
    } else if (b == kBins + 1) {
      std::snprintf(range, sizeof range, ">= %d", lo);
    } else {
      std::snprintf(range, sizeof range, "[%d, %d)", lo, lo + 1);
    }
    const auto bar = static_cast<int>(std::lround(40.0 * static_cast<double>(n) / total));
    std::printf("    %-9s %6llu %5.1f%% %c %.*s\n", range, static_cast<unsigned long long>(n),
                100.0 * static_cast<double>(n) / total,
                (lo >= 0 && std::cmp_less(lo, window)) ? '*' : ' ', bar,
                "########################################");
  }
}

void printWait(const char* label, const Stats::ThrowerWait& w) {
  const uint64_t total = w.regainSeconds.size() + w.eliminatedFirst + w.roundEndedFirst;
  std::printf(
      "  %s: %llu throwers; regained ammo %zu, eliminated first %llu, round ended "
      "first %llu\n",
      label, static_cast<unsigned long long>(total), w.regainSeconds.size(),
      static_cast<unsigned long long>(w.eliminatedFirst),
      static_cast<unsigned long long>(w.roundEndedFirst));
  printDistribution("  seconds until ammo again", w.regainSeconds, "s");
}

}  // namespace

void Stats::merge(const Stats& o) {
  matches += o.matches;
  matchesFinished += o.matchesFinished;
  ticks += o.ticks;
  playingTicks += o.playingTicks;
  append(roundSeconds, o.roundSeconds);
  draws += o.draws;
  throws += o.throws;
  hits += o.hits;
  eliminations += o.eliminations;
  dashAttackWithAmmo += o.dashAttackWithAmmo;
  dashAttackWithoutAmmo += o.dashAttackWithoutAmmo;
  dashHits += o.dashHits;
  dashEliminations += o.dashEliminations;
  dashes += o.dashes;
  windowsOpened += o.windowsOpened;
  perfectCatches += o.perfectCatches;
  perfectCatchesOwn += o.perfectCatchesOwn;
  autoCatches += o.autoCatches;
  attemptDashes += o.attemptDashes;
  convertedDashes += o.convertedDashes;
  consultedContacts += o.consultedContacts;
  declinedContacts += o.declinedContacts;
  fullAmmoDeclined += o.fullAmmoDeclined;
  fullAmmoDeclinedInWindow += o.fullAmmoDeclinedInWindow;
  append(consultedDelta, o.consultedDelta);
  dangerContacts += o.dangerContacts;
  dangerContactsRecentDash += o.dangerContactsRecentDash;
  append(dangerDelta, o.dangerDelta);
  append(flightToPerfect, o.flightToPerfect);
  append(flightToAuto, o.flightToAuto);
  append(flightToHit, o.flightToHit);
  append(flightToGround, o.flightToGround);
  flightUnresolved += o.flightUnresolved;
  aliveTicks += o.aliveTicks;
  emptyTicks += o.emptyTicks;
  append(emptySpellSeconds, o.emptySpellSeconds);
  emptySpellsCensored += o.emptySpellsCensored;
  perfectAtFullAmmo += o.perfectAtFullAmmo;
  perfectBelowFullAmmo += o.perfectBelowFullAmmo;
  autoAtFullAmmo += o.autoAtFullAmmo;
  eliminatedHoldingAmmo += o.eliminatedHoldingAmmo;
  roundsSupplyExhausted += o.roundsSupplyExhausted;
  append(roundMinSupplyShare, o.roundMinSupplyShare);
  mergeWait(afterFullCatch, o.afterFullCatch);
  mergeWait(afterBelowFullCatch, o.afterBelowFullCatch);
  powerUpsSpawned += o.powerUpsSpawned;
  powerUpsTaken += o.powerUpsTaken;
  shieldsBroken += o.shieldsBroken;
  blinks += o.blinks;
  slams += o.slams;
  slammed += o.slammed;
  parryPresses += o.parryPresses;
  parries += o.parries;
}

void StatsCollector::beginContacts(const WorldState& s) {
  ownerBefore_.clear();
  ammoBefore_.fill(0);
  s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (p.id.isValid() && p.id.value < kMaxPlayers) ammoBefore_[p.id.value] = p.ammo;
  });
  s.boomerangs.forEachAlive(
      [&](EntityId id, const Boomerang& b) { ownerBefore_[keyOf(id)] = b.owner; });
}

void StatsCollector::observeContact(const WorldState& s, const Contact& c) {
  if (c.kindA == EntityKind::Player && c.kindB == EntityKind::Player) {
    // Row 25 of the rules table (rules/table.cpp), as InteractionSystem sees it.
    const Player* attacker = s.players.get(c.a);
    const Player* victim = s.players.get(c.b);
    if (attacker == nullptr || victim == nullptr) return;
    const Tags attackerForbid = Tags::of(Tag::Eliminated) | Tag::HitStun;
    const Tags victimForbid = attackerForbid | Tag::Invulnerable;
    if (!attacker->tags.has(Tag::Dashing) || !attacker->tags.hasNone(attackerForbid) ||
        !victim->tags.hasNone(victimForbid)) {
      return;
    }
    const std::pair<uint8_t, uint32_t> dash{
        static_cast<uint8_t>(attacker->id.value),
        attacker->abilities[kAbilitySlotDash].activatedTick.value};
    if (attacker->ammo >= defaultContent().dashAttack.minAmmo) {
      dashAttackWithAmmo_.insert(dash);
    } else {
      dashAttackWithoutAmmo_.insert(dash);
    }
    return;
  }
  if (c.kindA != EntityKind::Player || c.kindB != EntityKind::Boomerang) return;
  const Player* p = s.players.get(c.a);
  const Boomerang* b = s.boomerangs.get(c.b);
  if (p == nullptr || b == nullptr || p->tags.has(Tag::Eliminated)) return;
  if (b->phase == BoomerangPhase::Grounded) return;
  const ContentDB& content = defaultContent();
  const bool own = b->owner == p->id;
  const AbilityState& dash = p->abilities[kAbilitySlotDash];
  const double delta = static_cast<double>(s.tick.value) + c.tHit.toDouble() - pressTime(dash);

  // Rows 10 and 11 of the rules table (rules/table.cpp).
  const bool catchable = !own || b->phase == BoomerangPhase::Returning;
  if (catchable && p->tags.has(Tag::PerfectDash)) {
    const uint16_t window = content.abilityAt(dash.defIndex).perfectWindowTicks;
    const bool inWindow = delta >= 0.0 && delta < static_cast<double>(window);
    if (p->ammo >= p->stats.ammoMax) {  // the catch declines first on ammo (rules/catch.cpp)
      ++stats_.fullAmmoDeclined;
      if (inWindow) ++stats_.fullAmmoDeclinedInWindow;
    } else {
      ++stats_.consultedContacts;
      stats_.consultedDelta.push_back(delta);
      if (!inWindow) ++stats_.declinedContacts;
      attempted_.emplace(static_cast<uint8_t>(p->id.value), dash.activatedTick.value);
    }
  }
  if (!own) {
    ++stats_.dangerContacts;
    if (dash.phase != AbilityPhase::Idle || dash.cooldownTicks > 0) {
      ++stats_.dangerContactsRecentDash;
      stats_.dangerDelta.push_back(delta);
    }
  }
}

void StatsCollector::endRoundTracking(const WorldState& s) {
  if (roundPlayers_ > 0) {
    stats_.roundMinSupplyShare.push_back(static_cast<double>(roundMinSupply_) /
                                         static_cast<double>(roundPlayers_));
    if (roundExhausted_) ++stats_.roundsSupplyExhausted;
  }
  roundPlayers_ = 0;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (emptySince_[i]) {
      ++stats_.emptySpellsCensored;
      emptySince_[i].reset();
    }
    if (std::optional<Wait>& wait = throwerWait_[i]; wait.has_value()) {
      Stats::ThrowerWait& w = wait->full ? stats_.afterFullCatch : stats_.afterBelowFullCatch;
      if (s.playerBySlot(PlayerId{i}) == nullptr) {
        ++w.eliminatedFirst;
      } else {
        ++w.roundEndedFirst;
      }
      wait.reset();
    }
  }
}

void StatsCollector::afterStep(const WorldState& s, const EventSink& sink) {
  const Tick stepped{s.tick.value - 1};
  ++stats_.ticks;
  // PlayerEliminated directly follows the hit that caused it (rules/apply_damage.hpp).
  EventKind lastHit = EventKind::None;
  PlayerId lastVictim = PlayerId::invalid();
  for (const SimEvent& e : sink) {
    switch (e.kind) {
      case EventKind::RoundStarted:
        stats_.flightUnresolved += inFlight_.size();
        inFlight_.clear();
        break;
      case EventKind::RoundEnded:
        stats_.roundSeconds.push_back(seconds(playingStart_, e.tick));
        if (!e.player.isValid()) ++stats_.draws;
        break;
      case EventKind::MatchEnded:
        ++stats_.matchesFinished;
        break;
      case EventKind::PlayerDashed:
        ++stats_.dashes;
        break;
      case EventKind::BoomerangThrown:
        ++stats_.throws;
        inFlight_[keyOf(e.entity)] = e.tick;
        break;
      case EventKind::BoomerangCaught: {
        const auto how = static_cast<CatchHow>(e.a);
        const uint8_t catcher = static_cast<uint8_t>(e.player.value);
        const Player* p = s.playerBySlot(e.player);
        const uint8_t ammoMax = p != nullptr ? p->stats.ammoMax : 1;
        const bool full = catcher < kMaxPlayers && ammoBefore_[catcher] >= ammoMax;
        const auto flight = inFlight_.find(keyOf(e.entity));
        if (flight != inFlight_.end()) {
          (how == CatchHow::Perfect ? stats_.flightToPerfect : stats_.flightToAuto)
              .push_back(seconds(flight->second, e.tick));
          inFlight_.erase(flight);
        }
        if (how == CatchHow::Auto) {
          ++stats_.autoCatches;
          if (full) ++stats_.autoAtFullAmmo;
          break;
        }
        ++stats_.perfectCatches;
        ++(full ? stats_.perfectAtFullAmmo : stats_.perfectBelowFullAmmo);
        if (p != nullptr) {
          converted_.emplace(catcher, p->abilities[kAbilitySlotDash].activatedTick.value);
        }
        const auto owner = ownerBefore_.find(keyOf(e.entity));
        if (owner == ownerBefore_.end()) break;
        if (owner->second == e.player) {
          ++stats_.perfectCatchesOwn;
        } else if (const Player* thrower = s.playerBySlot(owner->second);
                   thrower != nullptr && thrower->ammo == 0) {
          throwerWait_[owner->second.value] = Wait{e.tick, full};
        }
        break;
      }
      case EventKind::BoomerangGrounded: {
        const auto flight = inFlight_.find(keyOf(e.entity));
        if (flight != inFlight_.end()) {
          stats_.flightToGround.push_back(seconds(flight->second, e.tick));
          inFlight_.erase(flight);
        }
        break;
      }
      case EventKind::PlayerDashHit:
        ++stats_.dashHits;
        lastHit = e.kind;
        lastVictim = e.player;
        break;
      case EventKind::PlayerHit: {
        ++stats_.hits;
        lastHit = e.kind;
        lastVictim = e.player;
        const auto flight = inFlight_.find(keyOf(e.entity));
        if (flight != inFlight_.end()) {
          stats_.flightToHit.push_back(seconds(flight->second, e.tick));
          inFlight_.erase(flight);
        }
        break;
      }
      case EventKind::PlayerEliminated:
        ++stats_.eliminations;
        if (lastHit == EventKind::PlayerDashHit && lastVictim == e.player) {
          ++stats_.dashEliminations;
        }
        if (e.player.isValid() && e.player.value < kMaxPlayers && ammoBefore_[e.player.value] > 0) {
          ++stats_.eliminatedHoldingAmmo;
        }
        break;
      case EventKind::PickupSpawned:
        if (e.a == static_cast<uint32_t>(PickupKind::PowerUp)) ++stats_.powerUpsSpawned;
        break;
      case EventKind::PickupTaken:
        if (e.a == static_cast<uint32_t>(PickupKind::PowerUp)) ++stats_.powerUpsTaken;
        break;
      case EventKind::ShieldBroken:
        ++stats_.shieldsBroken;
        break;
      case EventKind::PlayerBlinked:
        ++stats_.blinks;
        break;
      case EventKind::SlamLanded:
        ++stats_.slams;
        break;
      case EventKind::PlayerSlammed:
        ++stats_.slammed;
        break;
      case EventKind::PlayerParryStarted:
        ++stats_.parryPresses;
        break;
      case EventKind::BoomerangParried:
        ++stats_.parries;
        break;
      default:
        break;
    }
  }

  const MatchPhase phase = s.rules.phase;
  if (phase == MatchPhase::Playing && prevPhase_ != MatchPhase::Playing) playingStart_ = stepped;
  if (prevPhase_ == MatchPhase::Playing && phase != MatchPhase::Playing) endRoundTracking(s);
  prevPhase_ = phase;

  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    const Player* p = s.playerBySlot(PlayerId{i});
    const bool tag = p != nullptr && p->tags.has(Tag::PerfectDash);
    if (tag && !perfectTag_[i]) ++stats_.windowsOpened;
    perfectTag_[i] = tag;
    if (phase != MatchPhase::Playing) continue;

    std::optional<Tick>& emptySince = emptySince_[i];
    std::optional<Wait>& wait = throwerWait_[i];
    if (p == nullptr) {  // eliminated this round
      if (emptySince.has_value()) {
        ++stats_.emptySpellsCensored;
        emptySince.reset();
      }
      if (wait.has_value()) {
        ++(wait->full ? stats_.afterFullCatch : stats_.afterBelowFullCatch).eliminatedFirst;
        wait.reset();
      }
      continue;
    }
    ++stats_.aliveTicks;
    if (p->ammo == 0) {
      ++stats_.emptyTicks;
      if (!emptySince.has_value()) emptySince = stepped;
    } else {
      if (emptySince.has_value()) {
        stats_.emptySpellSeconds.push_back(seconds(*emptySince, stepped));
        emptySince.reset();
      }
      if (wait.has_value()) {
        (wait->full ? stats_.afterFullCatch : stats_.afterBelowFullCatch)
            .regainSeconds.push_back(seconds(wait->since, stepped));
        wait.reset();
      }
    }
  }
  if (phase == MatchPhase::Playing) {
    ++stats_.playingTicks;
    uint32_t supply = 0;
    uint32_t alive = 0;
    s.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
      supply += p.ammo;
      ++alive;
    });
    s.boomerangs.forEachAlive([&](EntityId /*id*/, const Boomerang& /*b*/) { ++supply; });
    // Banana pickups only: power-ups and capture flags (v0.33) are no ammo.
    s.pickups.forEachAlive([&](EntityId /*id*/, const Pickup& k) {
      if (k.kind == PickupKind::Banana) ++supply;
    });
    if (roundPlayers_ == 0) {  // first Playing tick of the round
      roundPlayers_ = alive;
      roundMinSupply_ = supply;
      roundExhausted_ = false;
    }
    roundMinSupply_ = std::min(roundMinSupply_, supply);
    if (supply == 0 && alive >= 2) roundExhausted_ = true;
  }
}

void StatsCollector::finish(const WorldState& s) {
  if (prevPhase_ == MatchPhase::Playing) endRoundTracking(s);
  stats_.flightUnresolved += inFlight_.size();
  inFlight_.clear();
  stats_.matches = 1;
  stats_.attemptDashes = attempted_.size();
  uint64_t converted = 0;
  for (const auto& d : converted_)
    if (attempted_.contains(d)) ++converted;
  stats_.convertedDashes = converted;
  stats_.dashAttackWithAmmo = dashAttackWithAmmo_.size();
  stats_.dashAttackWithoutAmmo = dashAttackWithoutAmmo_.size();
}

void statsStep(WorldState& s, const InputFrame& in, const MatchContent& content, EventSink& sink,
               StatsCollector& collector) {
  // The order of step() (step.cpp, architecture §5.3); callers compare the
  // checksum with a step() shadow every tick.
  rulesPreTick(s, content, sink);
  inputSystem(s, content, in);
  abilitySystem(s, content, sink);
  movementSystem(s, content);
  projectileSystem(s, content, sink);
  Contacts contacts;
  collisionSystem(s, content, contacts);
  collector.beginContacts(s);
  // InteractionSystem one oriented contact at a time, in its resolution order, so each
  // contact is judged against the state the rules table would see: a contact
  // whose banana was caught, or whose player was eliminated, earlier in the
  // same tick is not counted as consulted. The shadow checksum proves the
  // split resolution matches interactionSystem() over the whole list.
  if (s.rules.phase == MatchPhase::Playing) {
    for (const Contact& c : interactionOrder(s, contacts)) {
      if (!entityAlive(s, c.kindA, c.a) || !entityAlive(s, c.kindB, c.b)) continue;
      collector.observeContact(s, c);
      interactionResolveContact(s, content, c, sink);
    }
  }
  modifierSystem(s, content);
  lifecycleSystem(s, content, sink);
  rulesPostTick(s, content, sink);
  ++s.tick;
}

void printMatchLine(uint32_t index, uint64_t seed, uint64_t inputSeed, const Stats& st) {
  const Summary rounds = summarise(st.roundSeconds);
  std::printf(
      "match %3u seed 0x%llx input 0x%llx: %6.1f s%s, %2zu rounds (mean %5.1f s), "
      "throws %3llu hits %3llu elim %3llu dashes %3llu, catches perfect %2llu auto %3llu, "
      "attempts %llu -> %llu, dash hits %llu\n",
      index, static_cast<unsigned long long>(seed), static_cast<unsigned long long>(inputSeed),
      static_cast<double>(st.ticks) * kTickSeconds, st.matchesFinished > 0 ? "" : " (cap)",
      rounds.n, rounds.mean, static_cast<unsigned long long>(st.throws),
      static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.eliminations),
      static_cast<unsigned long long>(st.dashes),
      static_cast<unsigned long long>(st.perfectCatches),
      static_cast<unsigned long long>(st.autoCatches),
      static_cast<unsigned long long>(st.attemptDashes),
      static_cast<unsigned long long>(st.convertedDashes),
      static_cast<unsigned long long>(st.dashHits));
}

void printSummary(const Stats& st, uint32_t players) {
  const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  const double rounds = static_cast<double>(std::max<size_t>(st.roundSeconds.size(), 1));
  size_t within5 = 0;
  size_t under3 = 0;
  size_t over60 = 0;
  for (const double r : st.roundSeconds) {
    within5 += r <= 5.0 ? 1 : 0;
    under3 += r < 3.0 ? 1 : 0;
    over60 += r > 60.0 ? 1 : 0;
  }

  std::printf("\n== %u players, %u matches (%u finished), %.1f min simulated, %.1f min playing\n",
              players, st.matches, st.matchesFinished,
              static_cast<double>(st.ticks) * kTickSeconds / 60.0,
              static_cast<double>(st.playingTicks) * kTickSeconds / 60.0);
  std::printf(
      "rounds: %zu (%u draws); decided within 5 s %.1f%%, under 3 s %.1f%%, over 60 s "
      "%.1f%%\n",
      st.roundSeconds.size(), st.draws, 100.0 * static_cast<double>(within5) / rounds,
      100.0 * static_cast<double>(under3) / rounds, 100.0 * static_cast<double>(over60) / rounds);
  printDistribution("round length", st.roundSeconds, "s");
  std::printf(
      "per round: throws %.1f, hits %.1f, eliminations %.1f, dashes %.1f, perfect "
      "windows %.1f, perfect catches %.2f, auto catches %.1f\n",
      static_cast<double>(st.throws) / rounds, static_cast<double>(st.hits) / rounds,
      static_cast<double>(st.eliminations) / rounds, static_cast<double>(st.dashes) / rounds,
      static_cast<double>(st.windowsOpened) / rounds,
      static_cast<double>(st.perfectCatches) / rounds,
      static_cast<double>(st.autoCatches) / rounds);
  std::printf(
      "totals: throws %llu, hits %llu, eliminations %llu, dashes %llu, perfect windows "
      "%llu, perfect catches %llu (own %llu), auto catches %llu\n",
      u(st.throws), u(st.hits), u(st.eliminations), u(st.dashes), u(st.windowsOpened),
      u(st.perfectCatches), u(st.perfectCatchesOwn), u(st.autoCatches));
  std::printf("hit rate: %.1f%% of throws hit a player\n", 100.0 * ratio(st.hits, st.throws));
  std::printf(
      "dash attacks (dashes that met a hittable player): with ammo %llu, without ammo %llu; "
      "dash hits %llu (%.2f per round), eliminations by dash %llu\n",
      u(st.dashAttackWithAmmo), u(st.dashAttackWithoutAmmo), u(st.dashHits),
      static_cast<double>(st.dashHits) / rounds, u(st.dashEliminations));

  std::printf(
      "catch attempts (dashes below ammoMax whose window met a catchable banana): %llu, "
      "converted %llu (%.1f%%); consulted contacts %llu, declined at the sub-tick edge %llu\n",
      u(st.attemptDashes), u(st.convertedDashes),
      100.0 * ratio(st.convertedDashes, st.attemptDashes), u(st.consultedContacts),
      u(st.declinedContacts));
  std::printf(
      "catch declined at full ammo (v0.16): %llu contacts, %llu of them inside the perfect "
      "window\n",
      u(st.fullAmmoDeclined), u(st.fullAmmoDeclinedInWindow));
  std::printf(
      "danger contacts (non-own banana in flight touches a live player): %llu, %llu "
      "(%.1f%%) within a dash's cooldown\n",
      u(st.dangerContacts), u(st.dangerContactsRecentDash),
      100.0 * ratio(st.dangerContactsRecentDash, st.dangerContacts));
  printDeltaHistogram("consulted contacts", st.consultedDelta, dash.perfectWindowTicks);
  printDeltaHistogram("danger contacts after a dash", st.dangerDelta, dash.perfectWindowTicks);

  std::printf("banana flight, seconds from the throw:\n");
  printDistribution("to a perfect catch", st.flightToPerfect, "s");
  printDistribution("to an auto catch (back home)", st.flightToAuto, "s");
  printDistribution("to a hit", st.flightToHit, "s");
  printDistribution("to grounding without a hit", st.flightToGround, "s");
  std::printf("  still in the air at round end: %llu\n", u(st.flightUnresolved));

  std::printf("ammo: players spend %.1f%% of their playing time at 0 ammo\n",
              100.0 * ratio(st.emptyTicks, st.aliveTicks));
  printDistribution("empty spell (ammo came back)", st.emptySpellSeconds, "s");
  std::printf("  empty spells cut short by elimination or round end: %llu\n",
              u(st.emptySpellsCensored));

  std::printf(
      "perfect catch at full ammo (0 since v0.16): %llu of %llu perfect catches "
      "(%.1f%%); auto catches at full ammo %llu\n",
      u(st.perfectAtFullAmmo), u(st.perfectCatches),
      100.0 * ratio(st.perfectAtFullAmmo, st.perfectCatches), u(st.autoAtFullAmmo));
  printWait("thrower after a full-ammo catch of its banana", st.afterFullCatch);
  printWait("thrower after a below-full catch of its banana", st.afterBelowFullCatch);
  std::printf(
      "banana supply (held + in flight + pickups): eliminated while holding ammo %llu; "
      "rounds where it hit 0 with 2+ players alive: %u of %zu\n",
      u(st.eliminatedHoldingAmmo), st.roundsSupplyExhausted, st.roundMinSupplyShare.size());
  printDistribution("lowest supply / players, per round", st.roundMinSupplyShare, "");
  std::printf("power-ups (§5.8): spawned %llu, taken %llu, shields broken %llu\n",
              u(st.powerUpsSpawned), u(st.powerUpsTaken), u(st.shieldsBroken));
  std::printf(
      "abilities (v0.21): blinks %llu, slams %llu (monkeys pushed %llu), parries %llu of %llu "
      "presses\n",
      u(st.blinks), u(st.slams), u(st.slammed), u(st.parries), u(st.parryPresses));
}

}  // namespace kfm::replay
