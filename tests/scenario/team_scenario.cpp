// Teams (architecture §5.3, §5.6, §5.10, v0.32): MatchConfig::teams through
// MatchContent::relation, the rules table's Ally/Enemy rows and the
// friendly-fire tag a mode's baseline grants, the team kinds (team_rounds,
// team_deathmatch), the per-role loadouts and the arena's spawn groups.
#include "scenario_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>

using namespace kfm;
using namespace kfm::sim;
using namespace kfm::sim::test;

namespace {

constexpr uint8_t kTeamRounds = 1;
constexpr uint8_t kTeamDeathmatch = 2;
constexpr uint8_t kTeamRoundsFf = 3;  // team rounds with friendly fire in the baseline
constexpr uint8_t kTeamRoles = 4;     // team rounds handing out two roles
constexpr uint8_t kVersus = 1;        // the dojo with two spawn groups
constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};
constexpr PlayerId kP2{2};
constexpr PlayerId kP3{3};
// Two teams over four slots: {0, 2} and {1, 3}.
constexpr TeamSet kTwoTeams = balancedTeams(2, 0b1111);

ContentDB teamContent() {
  ContentDB c = defaultContent();
  const uint8_t ff = addPowerUp(c, "friendly_fire", 1, {tagGrant(Tag::FriendlyFire)});
  ArenaDef& versus = c.arenas[kVersus];
  versus = c.arenas[0];
  versus.name = ContentName::of("versus");
  versus.spawnGroups = 2;
  c.arenaCount = 2;
  ModeDef& tr = c.modes[kTeamRounds];
  tr.name = ContentName::of("team_rounds");
  tr.kind = ModeKind::TeamRounds;
  tr.teams = 2;
  tr.roundsToWin = 2;
  tr.roundStartTicks = 10;
  tr.roundEndTicks = 20;
  ModeDef& td = c.modes[kTeamDeathmatch];
  td.name = ContentName::of("team_deathmatch");
  td.kind = ModeKind::TeamDeathmatch;
  td.teams = 2;
  td.baseline = ff;  // so an ally kill can happen at all
  td.roundStartTicks = 10;
  td.scoreToWin = 3;
  td.timeLimitTicks = 6000;
  td.respawnTicks = 20;
  td.respawnInvulnTicks = 0;
  c.modes[kTeamRoundsFf] = tr;
  c.modes[kTeamRoundsFf].name = ContentName::of("team_rounds_ff");
  c.modes[kTeamRoundsFf].baseline = ff;
  c.modes[kTeamRoles] = tr;
  c.modes[kTeamRoles].name = ContentName::of("team_roles");
  c.modes[kTeamRoles].roles[0] = 0;
  c.modes[kTeamRoles].roles[1] = 1;
  c.modes[kTeamRoles].roleCount = 2;
  c.modeCount = 5;
  // Loadout 1: a throw on the ability button too (any kit tells the roles apart).
  c.loadouts[1].name = ContentName::of("double");
  c.loadouts[1].abilities = {c.player.abilities[0], c.player.abilities[1], c.player.abilities[1]};
  c.loadoutCount = 2;
  return c;
}

const ContentDB& content() {
  static const ContentDB c = teamContent();
  return c;
}

MatchConfig teamConfig(uint8_t mode, PlayerMask players = 0b1111, uint64_t seed = 0x7EA4,
                       uint8_t arena = kVersus) {
  return MatchConfig{.seed = seed,
                     .players = players,
                     .arena = arena,
                     .mode = mode,
                     .teams = teamsFor(content().modeAt(mode), players)};
}

WorldState playing(const MatchConfig& config, EventLog& log) {
  WorldState s = initialWorld(config);
  const MatchContent m = matchContent(content(), config);
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
  return s;
}

// Puts every monkey on its own spot, far from the others.
void spreadOut(WorldState& s) {
  const std::array<Vec2, 4> spots = {
      Vec2{Fixed::fromInt(-12), Fixed::fromInt(5)}, Vec2{Fixed::fromInt(12), Fixed::fromInt(5)},
      Vec2{Fixed::fromInt(-12), Fixed::fromInt(-5)}, Vec2{Fixed::fromInt(12), Fixed::fromInt(-5)}};
  for (uint8_t i = 0; i < 4; ++i) {
    if (s.playerBySlot(PlayerId{i}) != nullptr) placePlayer(s, PlayerId{i}, spots[i]);
  }
}

// Fires a banana owned by `owner` into `victim` from one unit to its left
// and steps until it lands or passes (10 ticks). Returns whether it hit.
bool shoot(WorldState& s, const MatchContent& m, PlayerId owner, PlayerId victim, EventLog& log) {
  const Vec2 at = playerAt(s, victim).transform.pos;
  spawnBanana(s, owner, at - Vec2{Fixed::one(), Fixed::zero()},
              Vec2{Fixed::fromRatio(1, 2), Fixed::zero()}, BoomerangPhase::Outbound, m.db);
  const size_t before = log.size();
  bool hit = false;
  for (int t = 0; t < 10; ++t) {
    runTicks(s, frameFor(s), 1, m, log);
    for (size_t i = before; i < log.size(); ++i) {
      if (log[i].kind == EventKind::PlayerHit && log[i].player == victim) hit = true;
    }
    if (hit || s.rules.phase != MatchPhase::Playing) break;
  }
  return hit;
}

// Steps until the phase is Playing again (a new round).
void untilPlaying(WorldState& s, const MatchContent& m, EventLog& log) {
  for (int i = 0; i < 1000 && s.rules.phase != MatchPhase::Playing; ++i) {
    runTicks(s, frameFor(s), 1, m, log);
  }
  REQUIRE(s.rules.phase == MatchPhase::Playing);
}

}  // namespace

TEST_CASE("teams: balanced assignment and the relation", "[scenario][teams]") {
  STATIC_REQUIRE(kTwoTeams == TeamSet{0, 1, 0, 1, 0, 1, 0, 1, 0, 1});
  // Present slots round-robin, then every other slot to the smallest team.
  CHECK(balancedTeams(2, 0b1001) == TeamSet{0, 0, 1, 1, 0, 1, 0, 1, 0, 1});
  CHECK(balancedTeams(3, 0b111) == TeamSet{0, 1, 2, 0, 1, 2, 0, 1, 2, 0});
  CHECK(balancedTeams(0, 0b1111) == kFreeForAllTeams);
  CHECK(balancedTeams(1, 0b1111) == kFreeForAllTeams);
  const MatchContent ffa(content());
  CHECK(ffa.relation(kP0, kP0) == Relation::Self);
  CHECK(ffa.relation(kP0, kP2) == Relation::Enemy);
  CHECK(ffa.relation(kP0, PlayerId::invalid()) == Relation::Enemy);
  const MatchContent teams = matchContent(content(), teamConfig(kTeamRounds));
  CHECK(teams.relation(kP0, kP2) == Relation::Ally);
  CHECK(teams.relation(kP3, kP1) == Relation::Ally);
  CHECK(teams.relation(kP0, kP1) == Relation::Enemy);
  std::array<int8_t, kMaxPlayers> scores{};
  scores = {1, 2, 3, 4};
  CHECK(teams.teamScore(scores, 0) == 4);
  CHECK(teams.teamScore(scores, 1) == 6);
  CHECK(ffa.teamScore(scores, 3) == 4);  // a free-for-all team is one slot
}

TEST_CASE("teams: an ally's banana passes through, an enemy's hits", "[scenario][teams]") {
  const MatchConfig config = teamConfig(kTeamRounds);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  spreadOut(s);
  CHECK_FALSE(shoot(s, m, kP0, kP2, log));  // P0 and P2 are allies
  CHECK(s.playerBySlot(kP2) != nullptr);
  CHECK(countEvents(log, EventKind::PlayerHit) == 0);
  CHECK(shoot(s, m, kP1, kP2, log));  // P1 is an enemy
  CHECK(firstEvent(log, EventKind::PlayerEliminated).player == kP2);
}

TEST_CASE("teams: a friendly-fire baseline lets an ally's banana hit", "[scenario][teams]") {
  const MatchConfig config = teamConfig(kTeamRoundsFf);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  // The baseline is a permanent ModifierList entry granted at the spawn.
  const Player& p2 = playerAt(s, kP2);
  REQUIRE(p2.modifiers.count == 1);
  CHECK(p2.modifiers.items[0].ticksLeft == kPermanentModifierTicks);
  CHECK(p2.tags.has(Tag::FriendlyFire));
  CHECK(p2.grantedTags.has(Tag::FriendlyFire));
  // It never expires (the def's own duration is one tick).
  runTicks(s, frameFor(s), 300, m, log);
  CHECK(playerAt(s, kP2).modifiers.count == 1);
  CHECK(playerAt(s, kP2).tags.has(Tag::FriendlyFire));
  spreadOut(s);
  CHECK(shoot(s, m, kP0, kP2, log));
  const SimEvent& hit = firstEvent(log, EventKind::PlayerHit);
  CHECK(hit.player == kP2);
}

TEST_CASE("teams: a dash attack spares an ally unless friendly fire is on", "[scenario][teams]") {
  const auto run = [](uint8_t mode) {
    const MatchConfig config = teamConfig(mode);
    const MatchContent m = matchContent(content(), config);
    EventLog log;
    WorldState s = playing(config, log);
    spreadOut(s);
    placePlayer(s, kP0, Vec2{Fixed::fromInt(-2), Fixed::zero()});
    placePlayer(s, kP2, Vec2{Fixed::fromInt(2), Fixed::zero()});
    REQUIRE(playerAt(s, kP0).ammo >= content().dashAttack.minAmmo);
    log.clear();
    for (int k = 0; k < 30; ++k) {
      InputFrame f = frameFor(s);
      f.inputs[0].aimDir = 0;  // toward +x, at P2
      if (k == 0) f.inputs[0].buttons = buttonBit(Button::Dash);
      runTicks(s, f, 1, m, log);
    }
    return countEvents(log, EventKind::PlayerDashHit);
  };
  CHECK(run(kTeamRounds) == 0);
  CHECK(run(kTeamRoundsFf) == 1);
}

TEST_CASE("team rounds: the last team standing wins; team round wins take the match",
          "[scenario][teams][modes]") {
  const MatchConfig config = teamConfig(kTeamRounds);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  spreadOut(s);
  // One of team 1 down: two teams still stand.
  REQUIRE(shoot(s, m, kP0, kP1, log));
  runTicks(s, frameFor(s), 2, m, log);
  CHECK(s.rules.phase == MatchPhase::Playing);
  // The second: team 0 wins the round, credited to its lowest survivor.
  REQUIRE(shoot(s, m, kP2, kP3, log));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(countEvents(log, EventKind::RoundEnded) == 1);
  CHECK(firstEvent(log, EventKind::RoundEnded).player == kP0);
  CHECK(s.rules.scores[0] == 1);
  CHECK(m.teamScore(s.rules.scores, 0) == 1);
  CHECK(s.rules.phase == MatchPhase::RoundEnded);

  // Round two: P0 falls first, so P2 is credited; team 0's total of 2 wins.
  untilPlaying(s, m, log);
  spreadOut(s);
  REQUIRE(shoot(s, m, kP1, kP0, log));
  REQUIRE(shoot(s, m, kP2, kP1, log));
  REQUIRE(shoot(s, m, kP2, kP3, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.scores[0] == 1);
  CHECK(s.rules.scores[2] == 1);
  CHECK(m.teamScore(s.rules.scores, 0) == 2);
  REQUIRE(countEvents(log, EventKind::MatchEnded) == 1);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP2);
  CHECK(s.rules.phase == MatchPhase::MatchEnded);
}

TEST_CASE("team rounds: one team present never ends the round", "[scenario][teams][modes]") {
  // Slots 0 and 2 only, both on team 0 (the teams of all four slots, two left).
  MatchConfig config = teamConfig(kTeamRounds, 0b101);
  config.teams = kTwoTeams;
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  runTicks(s, frameFor(s), 120, m, log);
  CHECK(s.rules.phase == MatchPhase::Playing);
  CHECK(countEvents(log, EventKind::RoundEnded) == 0);
}

TEST_CASE("team deathmatch: enemy eliminations score for the team, an ally's does not",
          "[scenario][teams][modes]") {
  const MatchConfig config = teamConfig(kTeamDeathmatch);
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  spreadOut(s);
  // Friendly fire (the baseline) lets P0 eliminate its ally P2: no score.
  REQUIRE(shoot(s, m, kP0, kP2, log));
  runTicks(s, frameFor(s), 1, m, log);
  CHECK(s.rules.scores[0] == 0);
  CHECK(s.rules.deaths[2] == 1);
  CHECK(countEvents(log, EventKind::ScoreChanged) == 0);
  // Enemy eliminations score per slot; the team's score is the sum.
  REQUIRE(shoot(s, m, kP0, kP1, log));
  runTicks(s, frameFor(s), 30, m, log);  // past the respawns
  spreadOut(s);
  REQUIRE(shoot(s, m, kP2, kP3, log));
  runTicks(s, frameFor(s), 30, m, log);
  CHECK(s.rules.scores[0] == 1);
  CHECK(s.rules.scores[2] == 1);
  CHECK(m.teamScore(s.rules.scores, 0) == 2);
  CHECK(countEvents(log, EventKind::ScoreChanged) == 2);
  CHECK(s.rules.phase == MatchPhase::Playing);
  // The third reaches scoreToWin: the team's present top scorer wins.
  spreadOut(s);
  REQUIRE(shoot(s, m, kP2, kP1, log));
  runTicks(s, frameFor(s), 1, m, log);
  REQUIRE(countEvents(log, EventKind::MatchEnded) == 1);
  CHECK(firstEvent(log, EventKind::MatchEnded).player == kP2);
  CHECK(m.teamScore(s.rules.scores, 0) == 3);
}

TEST_CASE("spawn groups: every team starts and respawns in its own group",
          "[scenario][teams][spawn]") {
  const uint64_t seed = GENERATE(1u, 2u, 3u, 0xBEEFu, 0x7EA4u);
  const uint8_t mode = GENERATE(kTeamRounds, kTeamDeathmatch);
  const MatchConfig config = teamConfig(mode, 0b111111, seed);
  const MatchContent m = matchContent(content(), config);
  const ArenaDef& arena = m.arena;
  REQUIRE(arena.spawnGroups == 2);
  const auto groupOf = [&](Vec2 pos) {
    for (uint8_t k = 0; k < arena.spawnPointCount; ++k) {
      if (arena.spawnPoints[k] == pos) return k < arena.spawnPointCount / 2 ? 0 : 1;
    }
    return -1;
  };
  EventLog log;
  WorldState s = playing(config, log);
  for (uint8_t i = 0; i < 6; ++i) {
    INFO("slot " << int{i});
    CHECK(groupOf(playerAt(s, PlayerId{i}).transform.pos) == config.teams[i]);
  }
  // Three monkeys of a team spread over their group's five points.
  CHECK(playerAt(s, kP0).transform.pos != playerAt(s, kP2).transform.pos);
  if (mode == kTeamDeathmatch) {
    spreadOut(s);
    REQUIRE(shoot(s, m, kP0, kP1, log));
    runTicks(s, frameFor(s), 25, m, log);
    CHECK(groupOf(playerAt(s, kP1).transform.pos) == 1);
  }
}

TEST_CASE("spawn groups: a free-for-all alternates the groups by slot",
          "[scenario][teams][spawn]") {
  const MatchConfig config{.seed = 9, .players = 0b1111, .arena = kVersus};
  const MatchContent m = matchContent(content(), config);
  EventLog log;
  WorldState s = playing(config, log);
  for (uint8_t i = 0; i < 4; ++i) {
    const Vec2 pos = playerAt(s, PlayerId{i}).transform.pos;
    const SpawnRange r = spawnGroupRange(m.arena, i);
    bool inGroup = false;
    for (uint8_t k = r.begin; k < r.end; ++k)
      inGroup = inGroup || m.arena.spawnPoints[k] == pos;
    CHECK(inGroup);
  }
}

TEST_CASE("roles: a team mode hands out loadouts by position within the team",
          "[scenario][teams][roles]") {
  const MatchConfig config = teamConfig(kTeamRoles);
  const MatchContent m = matchContent(content(), config);
  // Team 0 = {0, 2}, team 1 = {1, 3}: the first of each plays role 0.
  CHECK(m.loadoutIndexFor(kP0) == 0);
  CHECK(m.loadoutIndexFor(kP1) == 0);
  CHECK(m.loadoutIndexFor(kP2) == 1);
  CHECK(m.loadoutIndexFor(kP3) == 1);
  CHECK(m.loadoutIndexFor(PlayerId{4}) == 0);  // round-robin
  EventLog log;
  WorldState s = playing(config, log);
  CHECK(playerAt(s, kP0).abilities[kAbilitySlotExtra].defIndex == 0);
  CHECK(playerAt(s, kP2).abilities[kAbilitySlotExtra].defIndex ==
        content().loadouts[1].abilities[kAbilitySlotExtra]);
  // A free-for-all mode ignores roles: the match's loadout.
  const MatchContent ffa = matchContent(content(), MatchConfig{.loadout = 1});
  CHECK(ffa.loadoutIndexFor(kP2) == 1);
}

TEST_CASE("compatible: a team mode needs a spawn group per team", "[scenario][teams]") {
  CHECK(content().compatible(0, 0));
  CHECK_FALSE(content().compatible(0, kTeamRounds));
  CHECK(content().compatible(kVersus, kTeamRounds));
  CHECK(content().firstCompatibleArena(kTeamRounds) == kVersus);
}
