// The reference bot (architecture §8) driven through AgentHost and sim::step:
// it beats the scripted generator, it is deterministic, every difficulty
// preset plays (throws, dashes, catches), a match of four reference bots
// decides its first round quickly with every gameplay event, and it uses
// the third-slot abilities when the loadout has them.
#include "kfm/ai/reference_bot.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

struct MatchResult {
  int botRounds{0};
  int otherRounds{0};
  int draws{0};
  int ticks{0};
  int firstRoundEnd{-1};  // ticks from the start to the first RoundEnded
  bool ended{false};
  std::map<EventKind, int> events;
  int perfectCatches{0};
  int botsKilledByOthers{0};  // eliminations of a bot by a non-bot
  int othersKilledByBots{0};
  int botsKilledByBots{0};
  std::vector<InputFrame> frames;
  std::vector<uint32_t> checksums;
};

[[nodiscard]] bool isBot(PlayerMask bots, uint32_t slot) {
  return slot < kMaxPlayers && maskHas(bots, PlayerId{static_cast<uint8_t>(slot)});
}

// Players in `players`; the slots in `bots` run ReferenceBot(config) through
// an AgentHost (no delay, bot versus bot), the others the scripted generator.
MatchResult playMatch(uint64_t seed, PlayerMask players, PlayerMask bots,
                      const ReferenceBotConfig& config, int maxTicks,
                      const MatchContent& content = defaultContent(), bool record = false) {
  WorldState s = initialWorld(MatchConfig{seed, players});
  AgentHost host;
  for (uint8_t i = 0; i < kMaxPlayers; ++i) {
    if (isBot(bots, i))
      host.attach(PlayerId{i}, std::make_unique<ReferenceBot>(config), seed * 31 + i);
  }
  Rng scripted = Rng::seeded(seed ^ 0x5C121Du);
  MatchResult r;
  EventSink sink;
  for (int k = 0; k < maxTicks && s.rules.phase != MatchPhase::MatchEnded; ++k) {
    InputFrame f = scriptedFrame(scripted, s, content);
    host.fill(f, s, content);
    sink.clear();
    step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    for (const SimEvent& e : sink) {
      ++r.events[e.kind];
      if (e.kind == EventKind::RoundEnded) {
        if (r.firstRoundEnd < 0) r.firstRoundEnd = k;
        if (!e.player.isValid()) {
          ++r.draws;
        } else if (isBot(bots, e.player.value)) {
          ++r.botRounds;
        } else {
          ++r.otherRounds;
        }
      }
      if (e.kind == EventKind::PlayerEliminated && e.a < kMaxPlayers) {
        const bool botKiller = isBot(bots, e.a);
        const bool botVictim = isBot(bots, e.player.value);
        if (botVictim && !botKiller) ++r.botsKilledByOthers;
        if (!botVictim && botKiller) ++r.othersKilledByBots;
        if (botVictim && botKiller) ++r.botsKilledByBots;
      }
      if (e.kind == EventKind::BoomerangCaught && e.a == static_cast<uint32_t>(CatchHow::Perfect)) {
        ++r.perfectCatches;
      }
    }
    if (record) {
      r.frames.push_back(f);
      r.checksums.push_back(checksum(s));
    }
    r.ticks = k + 1;
  }
  r.ended = s.rules.phase == MatchPhase::MatchEnded;
  return r;
}

[[nodiscard]] int eventCount(const MatchResult& r, EventKind kind) {
  const auto it = r.events.find(kind);
  return it == r.events.end() ? 0 : it->second;
}

// Context for the next assertion (shown when it fails).
void report(const char* name, const MatchResult& r) {
  UNSCOPED_INFO(name << ": rounds bot " << r.botRounds << " other " << r.otherRounds << " draw "
                     << r.draws << ", " << r.ticks << " ticks, first round end at "
                     << r.firstRoundEnd << ", perfect catches " << r.perfectCatches);
  for (const auto& [kind, n] : r.events)
    UNSCOPED_INFO("  event " << static_cast<int>(kind) << " x" << n);
}

}  // namespace

TEST_CASE("hard reference bots beat the scripted generator", "[ai][bot]") {
  // Ten free-for-all matches of four: two reference bots and two scripted
  // players, the bot slots rotating so no spawn point favours either side.
  // A scripted player takes a round when the two bots knock each other out
  // first, so the eliminations are the sharper measure.
  constexpr PlayerMask kFour = 0b1111;
  constexpr std::array<PlayerMask, 3> kBotSlots = {0b0101, 0b1010, 0b0011};
  MatchResult total;
  for (uint64_t m = 0; m < 10; ++m) {
    const MatchResult r = playMatch(1000 + m, kFour, kBotSlots[m % kBotSlots.size()],
                                    ReferenceBotConfig::hard(), 36000);
    report("match", r);
    CHECK(r.ended);
    total.botRounds += r.botRounds;
    total.otherRounds += r.otherRounds;
    total.draws += r.draws;
    total.othersKilledByBots += r.othersKilledByBots;
    total.botsKilledByOthers += r.botsKilledByOthers;
    total.botsKilledByBots += r.botsKilledByBots;
  }
  UNSCOPED_INFO("rounds won: reference bots "
                << total.botRounds << ", scripted " << total.otherRounds << ", draws "
                << total.draws << "; eliminations: scripted by bots " << total.othersKilledByBots
                << ", bots by scripted " << total.botsKilledByOthers << ", bots by bots "
                << total.botsKilledByBots);
  CHECK(total.botRounds >= 3 * total.otherRounds);
  CHECK(total.othersKilledByBots >= 5 * total.botsKilledByOthers);
}

TEST_CASE("the reference bot is deterministic", "[ai][bot]") {
  const MatchResult a =
      playMatch(77, 0b1111, 0b1111, ReferenceBotConfig::hard(), 2400, defaultContent(), true);
  const MatchResult b =
      playMatch(77, 0b1111, 0b1111, ReferenceBotConfig::hard(), 2400, defaultContent(), true);
  REQUIRE(a.frames.size() == b.frames.size());
  CHECK(a.frames == b.frames);
  CHECK(a.checksums == b.checksums);
  // Every press is a fresh edge: no button is held two ticks in a row.
  for (size_t k = 1; k < a.frames.size(); ++k) {
    for (uint32_t i = 0; i < 4; ++i) {
      INFO("tick " << k << " slot " << i);
      REQUIRE((a.frames[k].inputs[i].buttons & a.frames[k - 1].inputs[i].buttons) == 0);
    }
  }
  // A different seed plays differently (the bots draw from their own Rng).
  const MatchResult c =
      playMatch(78, 0b1111, 0b1111, ReferenceBotConfig::medium(), 2400, defaultContent(), true);
  CHECK(c.checksums != a.checksums);
}

TEST_CASE("every difficulty preset throws, dashes and catches", "[ai][bot]") {
  for (const auto& [name, config] : {std::pair{"easy", ReferenceBotConfig::easy()},
                                     std::pair{"medium", ReferenceBotConfig::medium()},
                                     std::pair{"hard", ReferenceBotConfig::hard()}}) {
    // Four bots of the preset against each other, then with scripted players.
    const MatchResult r = playMatch(4242, 0b1111, 0b1111, config, 18000);
    report(name, r);
    INFO("preset " << name);
    CHECK(eventCount(r, EventKind::BoomerangThrown) > 0);
    CHECK(eventCount(r, EventKind::PlayerDashed) > 0);
    CHECK(eventCount(r, EventKind::BoomerangCaught) > 0);
    CHECK(r.perfectCatches > 0);
    CHECK(eventCount(r, EventKind::PlayerEliminated) > 0);
  }
}

TEST_CASE("four reference bots decide the first round quickly", "[ai][bot]") {
  // dojo / rounds (the default content): the first round ends within ~30 s
  // and the match shows every gameplay event.
  for (const uint64_t seed : {1u, 2u, 3u}) {
    const MatchResult r = playMatch(seed, 0b1111, 0b1111, ReferenceBotConfig::hard(), 36000);
    report("all bots", r);
    INFO("seed " << seed);
    REQUIRE(r.firstRoundEnd >= 0);
    CHECK(r.firstRoundEnd <= 30 * static_cast<int>(kTickHz) + 60);
    CHECK(r.ended);
    for (const EventKind kind :
         {EventKind::BoomerangThrown, EventKind::PlayerHit, EventKind::PlayerEliminated,
          EventKind::BoomerangCaught, EventKind::RoundEnded, EventKind::PlayerDashed,
          EventKind::BoomerangBounced}) {
      INFO("event kind " << static_cast<int>(kind));
      CHECK(eventCount(r, kind) > 0);
    }
  }
}

TEST_CASE("two hard reference bots in a mirror mostly decide their rounds", "[ai][bot]") {
  // 1v1 on the dojo: mirrored spawns and no noise. Without the bots' own
  // Rng breaking the symmetry (ties, a standoff's throw hold) both throw on
  // one tick and fall on one tick round after round, and the match runs into
  // the tick cap; with it, most rounds have a winner.
  MatchResult total;
  for (uint64_t seed = 1; seed <= 5; ++seed) {
    const MatchResult r = playMatch(seed, 0b11, 0b11, ReferenceBotConfig::hard(), 36000);
    report("mirror", r);
    INFO("seed " << seed);
    CHECK(r.ended);
    total.botRounds += r.botRounds;
    total.draws += r.draws;
  }
  UNSCOPED_INFO("rounds with a winner " << total.botRounds << ", draws " << total.draws);
  CHECK(total.botRounds > total.draws);
}

namespace {

// The third-slot defs of content/abilities.toml.
[[nodiscard]] AbilityDef extraDef(AbilityKind kind) {
  AbilityDef d;
  d.kind = kind;
  switch (kind) {
    case AbilityKind::Blink:
      d.name = ContentName::of("blink");
      d.cooldownTicks = 90;
      d.startupTicks = 6;
      d.activeTicks = 10;
      d.recoveryTicks = 6;
      d.speed = Fixed::fromRatio(30, static_cast<int32_t>(kTickHz));
      break;
    case AbilityKind::Slam:
      d.name = ContentName::of("slam");
      d.cooldownTicks = 120;
      d.startupTicks = 12;
      d.activeTicks = 12;
      d.recoveryTicks = 12;
      d.param0 = Fixed::fromInt(3);
      d.param1 = Fixed::fromInt(4);
      break;
    default:
      d.name = ContentName::of("parry");
      d.cooldownTicks = 60;
      d.startupTicks = 2;
      d.activeTicks = 8;
      d.recoveryTicks = 14;
      break;
  }
  return d;
}

}  // namespace

TEST_CASE("the reference bot uses a third-slot ability", "[ai][bot]") {
  for (const auto& [kind, event] : {std::pair{AbilityKind::Blink, EventKind::PlayerBlinked},
                                    std::pair{AbilityKind::Slam, EventKind::SlamLanded},
                                    std::pair{AbilityKind::Parry, EventKind::BoomerangParried}}) {
    ContentDB c = defaultContent();
    c.abilities[c.abilityCount] = extraDef(kind);
    c.player.abilities[kAbilitySlotExtra] = c.abilityCount++;
    // Three matches: a landed parry needs a banana timed into the guard, and
    // one short match may not offer it.
    int events = 0;
    for (const uint64_t seed : {99u, 100u, 101u}) {
      const MatchResult r = playMatch(seed, 0b1111, 0b1111, ReferenceBotConfig::hard(), 18000, c);
      report("extra slot", r);
      INFO("ability kind " << static_cast<int>(kind) << " seed " << seed);
      CHECK(r.ended);
      events += eventCount(r, event);
    }
    INFO("ability kind " << static_cast<int>(kind));
    CHECK(events > 0);
  }
}
