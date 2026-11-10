// The reference bot (architecture §8) driven through AgentHost and sim::step:
// it beats the scripted generator, it is deterministic, every difficulty
// preset plays (throws, dashes, catches), a match of four reference bots
// decides its first round quickly with every gameplay event, and it uses
// the third-slot abilities when the loadout has them.
#include "kfm/ai/reference_bot.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/observation_builder.hpp"
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
    host.step(s, f, content, sink);
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
  // 3x, not 5x: over ten matches the ratio is noisy. Sweeping five seed
  // bases (50 matches each) gave 5.1x on average at a 120-tick round pause
  // and 5.5x at 180, but single bases ranged from 3.4x to 8.5x, so a 5x bar
  // sat on the mean and failed on any reshuffle of the Rng streams.
  CHECK(total.othersKilledByBots >= 3 * total.botsKilledByOthers);
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
    // Four bots of the preset against each other, pooled over three seeds:
    // the easy tier catches at 48/256 after an 18-tick reaction, so one
    // match is a thin sample of perfect catches.
    MatchResult r;
    for (const uint64_t seed : {4242u, 4243u, 4244u}) {
      const MatchResult m = playMatch(seed, 0b1111, 0b1111, config, 18000);
      for (const auto& [kind, n] : m.events)
        r.events[kind] += n;
      r.perfectCatches += m.perfectCatches;
    }
    report(name, r);
    INFO("preset " << name);
    CHECK(eventCount(r, EventKind::BoomerangThrown) > 0);
    CHECK(eventCount(r, EventKind::PlayerDashed) > 0);
    CHECK(eventCount(r, EventKind::BoomerangCaught) > 0);
    // A catch is planned for the press tick, and the reaction delay applies
    // the press that many ticks later uncompensated (the bot reacts late by
    // design), so only the zero-delay tier lands its perfect catches on
    // purpose; the others land them by luck, which three matches may not show.
    if (config.reactionDelayTicks == 0) CHECK(r.perfectCatches > 0);
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

TEST_CASE("an unarmed reference bot fetches a banana lying inside the wall band",
          "[ai][reference_bot][fetch]") {
  // Two bots on the dojo (half extents 16 x 9) stood at the edge of the
  // 2-unit wall band with no ammo, no banana in flight and the only bananas
  // lying 1.5 units inside the band, which is where a stalled client match
  // was found (the band penalty outweighed the fetch pull). Both must pick
  // one up and the round must end.
  const MatchContent content = defaultContent();
  WorldState s = makePlayingWorld(7, 0b11);
  for (uint8_t i = 0; i < 2; ++i) {
    Player& p =
        placePlayer(s, PlayerId{i}, Vec2{Fixed::fromInt(-14), Fixed::fromInt(i == 0 ? -7 : -1)});
    p.ammo = 0;
  }
  spawnPickup(s, Vec2{Fixed::fromRatio(-31, 2), Fixed::fromRatio(-17, 10)});
  spawnPickup(s, Vec2{Fixed::fromRatio(-156, 10), Fixed::fromRatio(-63, 10)});
  AgentHost host;
  for (uint8_t i = 0; i < 2; ++i) {
    host.attach(PlayerId{i}, std::make_unique<ReferenceBot>(ReferenceBotConfig::medium()),
                1000 + i);
  }
  EventSink sink;
  int taken = 0;
  bool roundEnded = false;
  for (int k = 0; k < 1800 && !roundEnded; ++k) {
    InputFrame f = frameFor(s);
    host.step(s, f, content, sink);
    for (const SimEvent& e : sink) {
      if (e.kind == EventKind::PickupTaken) ++taken;
      if (e.kind == EventKind::RoundEnded) roundEnded = true;
    }
  }
  CHECK(taken >= 1);
  CHECK(roundEnded);
}

TEST_CASE("the reference bot ignores a teammate's banana unless friendly fire is on (v0.32)",
          "[ai][bot][teams]") {
  // Slots 0 and 2 are allies, 1 and 3 their enemies. A banana flies at slot
  // 0 from close by; the bot dodges or catches an enemy's but walks on as if
  // a teammate's were not there.
  ContentDB c = defaultContent();
  const uint8_t ff = addPowerUp(c, "ff", 1, {tagGrant(Tag::FriendlyFire)});
  ModeDef& m = c.modes[1];
  m = c.modes[0];
  m.name = ContentName::of("team_rounds");
  m.kind = ModeKind::TeamRounds;
  m.teams = 2;
  c.modeCount = 2;
  const MatchConfig config{
      .seed = 7, .players = 0b1111, .mode = 1, .teams = balancedTeams(2, 0b1111)};
  const MatchContent match = matchContent(c, config);
  WorldState base = makePlayingWorld(7, 0b1111);
  placePlayer(base, PlayerId{0}, Vec2{Fixed::zero(), Fixed::zero()});
  placePlayer(base, PlayerId{1}, Vec2{Fixed::fromInt(12), Fixed::fromInt(6)});
  placePlayer(base, PlayerId{2}, Vec2{-Fixed::fromInt(12), Fixed::fromInt(6)});
  placePlayer(base, PlayerId{3}, Vec2{Fixed::fromInt(12), -Fixed::fromInt(6)});
  const auto decide = [&](PlayerId owner, bool friendlyFire) {
    WorldState s = base;
    if (owner.isValid()) {
      spawnBanana(s, owner, Vec2{-Fixed::fromInt(3), Fixed::zero()},
                  Vec2{Fixed::fromRatio(16, 60), Fixed::zero()});
    }
    if (friendlyFire) {
      playerAt(s, PlayerId{0}).tags.set(Tag::FriendlyFire);
      givePowerUp(playerAt(s, PlayerId{0}), ff, kPermanentModifierTicks);
    }
    ReferenceBot bot(ReferenceBotConfig{});
    bot.init(99, PlayerId{0});
    Observation obs{};
    buildObservation(s, match, PlayerId{0}, 0, obs);
    return bot.act(obs, s.tick).value_or(PlayerInput{});
  };
  const PlayerInput none = decide(PlayerId::invalid(), false);
  const PlayerInput ally = decide(PlayerId{2}, false);
  const PlayerInput enemy = decide(PlayerId{1}, false);
  CHECK(ally == none);
  CHECK_FALSE(enemy == none);
  // With friendly fire the teammate's banana is a threat like any other.
  CHECK_FALSE(decide(PlayerId{2}, true) == decide(PlayerId::invalid(), true));
}

namespace {

// The dojo with a pit strip running wall to wall at x in [minX, maxX]: the
// only way across is a dash (v0.57).
ContentDB pitStripContent(int32_t minX, int32_t maxX) {
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[0];
  a.hazards[0] = Hazard{Vec2{Fixed::fromInt(minX), -a.halfExtents.y},
                        Vec2{Fixed::fromInt(maxX), a.halfExtents.y}, HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  return c;
}

struct PitRun {
  int fell{0};
  int dashed{0};
  int taken{0};
};

// Slot 0 a hard reference bot without ammo at `from`, slot 1 idle far away
// without ammo, one banana lying at `banana`.
PitRun runPitBot(const ContentDB& c, Vec2 from, Vec2 banana, int ticks) {
  const MatchContent content = matchContent(c, MatchConfig{.seed = 7, .players = 0b11});
  WorldState s = makePlayingWorld(7, 0b11, c);
  placePlayer(s, PlayerId{0}, from).ammo = 0;
  placePlayer(s, PlayerId{1}, Vec2{Fixed::fromInt(-14), Fixed::fromInt(-7)}).ammo = 0;
  spawnPickup(s, banana);
  AgentHost host;
  host.attach(PlayerId{0}, std::make_unique<ReferenceBot>(ReferenceBotConfig::hard()), 1000);
  EventSink sink;
  PitRun r;
  for (int k = 0; k < ticks && r.taken == 0 && r.fell == 0; ++k) {
    InputFrame f = frameFor(s);
    host.step(s, f, content, sink);
    for (const SimEvent& e : sink) {
      if (e.player != PlayerId{0}) continue;
      if (e.kind == EventKind::PlayerFell) ++r.fell;
      if (e.kind == EventKind::PlayerDashed) ++r.dashed;
      if (e.kind == EventKind::PickupTaken) ++r.taken;
    }
  }
  return r;
}

}  // namespace

TEST_CASE("the reference bot keeps out of pits and dashes across a narrow one (v0.57)",
          "[ai][reference_bot][pit]") {
  SECTION("a 2-unit strip between it and the only banana: it dashes over") {
    const PitRun r = runPitBot(pitStripContent(-1, 1), Vec2{Fixed::fromInt(-6), Fixed::zero()},
                               Vec2{Fixed::fromInt(5), Fixed::zero()}, 900);
    CHECK(r.fell == 0);
    CHECK(r.dashed >= 1);
    CHECK(r.taken == 1);
  }
  SECTION("a banana lying in a wide pit is not worth a fall") {
    const PitRun r = runPitBot(pitStripContent(-3, 3), Vec2{Fixed::fromInt(-6), Fixed::zero()},
                               Vec2{Fixed::zero(), Fixed::zero()}, 600);
    CHECK(r.fell == 0);
    CHECK(r.taken == 0);
  }
}
