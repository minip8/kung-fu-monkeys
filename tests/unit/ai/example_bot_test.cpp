// The C SDK seed (tools/bots/sdk-c/kfm_bot.h) and its example bots
// (tools/bots/examples, architecture §8): the header's Observation layout and
// constants are the C++ ones, its integer math is the engine's bit for bit,
// its banana flight step reproduces the sim's ProjectileSystem exactly, and
// the checked-in chaser.wasm, built from the header, loads and plays a whole
// match against the reference bot within budget, deterministically, and the
// version 1 build of it kept as a fixture plays the same match (v0.80); the
// checked-in gatherer.wasm banks a harvest match to its score on a walled
// arena with trees, pits and two homes (v0.82). The win counts and the fuel
// and wall numbers are printed, never asserted.
#include "kfm/ai/agent_host.hpp"
#include "kfm/ai/observation.hpp"
#include "kfm/ai/observation_builder.hpp"
#include "kfm/ai/reference_bot.hpp"
#include "kfm/ai/wasm_runtime.hpp"
#include "kfm/core/fixed.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/core/vec2.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/tags.hpp"
#include "kfm/sim/world_state.hpp"
#include "systems/angle_of.hpp"

#include <catch2/catch_test_macros.hpp>

#include <kfm_bot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace kfm;
using namespace kfm::ai;
using namespace kfm::sim;

namespace {

// --- layout -------------------------------------------------------------------

static_assert(sizeof(kfm_observation) == sizeof(Observation));
static_assert(KFM_OBS_SIZE == sizeof(Observation));
static_assert(KFM_ABI_VERSION == kObservationVersion);
static_assert(KFM_ZONE_WORD == kObsZoneWord && KFM_ZONES == kObsZones);  // v0.69
// v0.80 (version 2): the zones block appended at version 1's end, and the
// control kind's modeData words (docs/bot-abi.md §3, modes/control.cpp).
static_assert(KFM_MAX_ZONES == kObsMaxZones);
static_assert(offsetof(kfm_observation, zones) == offsetof(Observation, zones));
static_assert(offsetof(kfm_observation, zones) == kObservationV1Size);
static_assert(sizeof(kfm_obs_zones) == sizeof(ObsZones));
static_assert(sizeof(kfm_obs_zone) == sizeof(ObsZone));
static_assert(KFM_MODE_CONTROL == static_cast<int>(ObsModeKind::Control));
static_assert(KFM_CONTROL_OWNER_WORD == 2 && KFM_CONTROL_CHALLENGER_WORD == 3 &&
              KFM_CONTROL_PROGRESS_WORD == 4);
static_assert(KFM_CONTROL_PROGRESS_WORD + ((KFM_MAX_ZONES + 1) / 2) <= KFM_ZONE_WORD,
              "the control state stays clear of the zone words");
// v0.82: the harvest kind, its beats word (modes/harvest.cpp) and the
// spawner's tree byte, within version 2.
static_assert(KFM_MODE_HARVEST == static_cast<int>(ObsModeKind::Harvest));
static_assert(KFM_HARVEST_BEATS_WORD == 0 && KFM_HARVEST_BEATS_WORD < KFM_ZONE_WORD);
static_assert(KFM_HARVEST_BANK_TICKS(0x00060002u) == 2 &&
              KFM_HARVEST_STEAL_TICKS(0x00060002u) == 6);
static_assert(KFM_HARVEST_BANK_TICKS(0xFFFF0000u) == 0 &&
              KFM_HARVEST_STEAL_TICKS(0x0000FFFFu) == 0);
static_assert(alignof(kfm_observation) == alignof(Observation));
#define KFM_SAME_AT(ct, cf, cppt) static_assert(offsetof(ct, cf) == offsetof(cppt, cf), #ct "." #cf)
// The SDK names a filled reserved field without the prefix (v0.35).
#define KFM_SAME_AS(ct, cf, cppt, cppf) \
  static_assert(offsetof(ct, cf) == offsetof(cppt, cppf), #ct "." #cf)
KFM_SAME_AT(kfm_observation, match, Observation);
KFM_SAME_AT(kfm_observation, rules, Observation);
KFM_SAME_AT(kfm_observation, self, Observation);
KFM_SAME_AT(kfm_observation, players, Observation);
KFM_SAME_AT(kfm_observation, bananas, Observation);
KFM_SAME_AT(kfm_observation, pickups, Observation);
KFM_SAME_AT(kfm_observation, arena, Observation);
KFM_SAME_AS(kfm_observation, heardCalls, Observation, reservedHeardCalls);
KFM_SAME_AT(kfm_obs_match, lastRoundWinner, ObsMatch);
KFM_SAME_AT(kfm_obs_match, respawnInvulnTicks, ObsMatch);
KFM_SAME_AS(kfm_obs_match, modeData, ObsMatch, reservedModeData);
KFM_SAME_AS(kfm_obs_match, turnTicks, ObsMatch, reservedTurnTicks);
KFM_SAME_AS(kfm_obs_match, ticksToLatch, ObsMatch, reservedTicksToLatch);
KFM_SAME_AS(kfm_obs_match, teamScores, ObsMatch, reservedTeamScores);
KFM_SAME_AT(kfm_obs_rules, bananaTurnRateBam, ObsRules);
KFM_SAME_AT(kfm_obs_rules, playerAmmoStart, ObsRules);
KFM_SAME_AT(kfm_obs_ability, activatedSubTick, ObsAbility);
KFM_SAME_AT(kfm_obs_ability, defPerfectWindowTicks, ObsAbility);
KFM_SAME_AT(kfm_obs_ability, dirY, ObsAbility);
KFM_SAME_AT(kfm_obs_self, lastButtons, ObsSelf);
KFM_SAME_AT(kfm_obs_self, radius, ObsSelf);
KFM_SAME_AT(kfm_obs_self, respawnTicks, ObsSelf);
KFM_SAME_AS(kfm_obs_self, cellX, ObsSelf, reservedCellX);
KFM_SAME_AS(kfm_obs_self, cellY, ObsSelf, reservedCellY);
KFM_SAME_AS(kfm_obs_self, facingOctant, ObsSelf, reservedFacingOctant);
KFM_SAME_AS(kfm_obs_self, role, ObsSelf, reservedRole);
KFM_SAME_AS(kfm_obs_self, team, ObsSelf, reservedTeam);
KFM_SAME_AS(kfm_obs_player, visible, ObsPlayer, reservedVisible);
KFM_SAME_AS(kfm_obs_player, team, ObsPlayer, reservedTeam);
KFM_SAME_AS(kfm_obs_banana, visible, ObsBanana, reservedVisible);
KFM_SAME_AT(kfm_obs_self, modifiers, ObsSelf);
KFM_SAME_AT(kfm_obs_player, dashing, ObsPlayer);
KFM_SAME_AT(kfm_obs_player, respawnTicks, ObsPlayer);
KFM_SAME_AT(kfm_obs_player, pad5, ObsPlayer);
KFM_SAME_AT(kfm_obs_banana, ownerRelation, ObsBanana);
KFM_SAME_AT(kfm_obs_banana, ticksInPhase, ObsBanana);
KFM_SAME_AT(kfm_obs_banana, radius, ObsBanana);
KFM_SAME_AT(kfm_obs_pickup, radius, ObsPickup);
KFM_SAME_AT(kfm_obs_hazard, pad2, ObsHazard);
KFM_SAME_AT(kfm_obs_spawner, intervalTicks, ObsSpawner);
KFM_SAME_AT(kfm_obs_spawner, maxAlive, ObsSpawner);
KFM_SAME_AT(kfm_obs_spawner, tree, ObsSpawner);
KFM_SAME_AT(kfm_obs_arena, spawnerCount, ObsArena);
KFM_SAME_AT(kfm_obs_arena, spawners, ObsArena);
KFM_SAME_AT(kfm_obs_heard_call, cellY, ObsHeardCall);
KFM_SAME_AT(kfm_obs_zones, count, ObsZones);
KFM_SAME_AT(kfm_obs_zones, zone, ObsZones);
KFM_SAME_AT(kfm_obs_zone, minX, ObsZone);
KFM_SAME_AT(kfm_obs_zone, minY, ObsZone);
KFM_SAME_AT(kfm_obs_zone, maxX, ObsZone);
KFM_SAME_AT(kfm_obs_zone, maxY, ObsZone);
#undef KFM_SAME_AT
#undef KFM_SAME_AS

static_assert(KFM_BTN_THROW == buttonBit(Button::Throw));
static_assert(KFM_BTN_DASH == buttonBit(Button::Dash));
static_assert(KFM_BTN_TAUNT == buttonBit(Button::Taunt));
static_assert(KFM_TAG_DASHING == static_cast<int>(Tag::Dashing));
static_assert(KFM_TAG_PERFECT_DASH == static_cast<int>(Tag::PerfectDash));
static_assert(KFM_TAG_INVULNERABLE == static_cast<int>(Tag::Invulnerable));
static_assert(KFM_TAG_PARRYING == static_cast<int>(Tag::Parrying));
static_assert(KFM_TAG_FRIENDLY_FIRE == static_cast<int>(Tag::FriendlyFire));
static_assert(KFM_TAG_CARRYING == static_cast<int>(Tag::Carrying));
static_assert(KFM_TAG_FALLING == static_cast<int>(Tag::Falling));
static_assert(KFM_SCHEMA_FLAG_FOG == kObsFlagFog);
static_assert(KFM_NO_CELL == kObsNoCell);
static_assert(KFM_MODE_CAPTURE == static_cast<int>(ObsModeKind::Capture));
static_assert(KFM_PICKUP_FLAG == static_cast<int>(ObsPickupKind::Flag));
static_assert(KFM_REL_ALLY == static_cast<int>(ObsRelation::Ally));
static_assert(KFM_REL_ENEMY == static_cast<int>(ObsRelation::Enemy));
static_assert(KFM_PHASE_PLAYING == static_cast<int>(ObsMatchPhase::Playing));
static_assert(KFM_PHASE_MATCH_ENDED == static_cast<int>(ObsMatchPhase::MatchEnded));
static_assert(KFM_MODE_SURVIVAL == static_cast<int>(ObsModeKind::Survival));
static_assert(KFM_MODE_TEAM_ROUNDS == static_cast<int>(ObsModeKind::TeamRounds));
static_assert(KFM_MODE_TEAM_DEATHMATCH == static_cast<int>(ObsModeKind::TeamDeathmatch));
static_assert(KFM_ABILITY_PARRY == static_cast<int>(ObsAbilityKind::Parry));
static_assert(KFM_ABILITY_CALL == static_cast<int>(ObsAbilityKind::Call));
static_assert(KFM_ABILITY_RECOVERY == static_cast<int>(ObsAbilityPhase::Recovery));
static_assert(KFM_BANANA_GROUNDED == static_cast<int>(ObsBananaPhase::Grounded));
static_assert(KFM_PICKUP_POWERUP == static_cast<int>(ObsPickupKind::PowerUp));
static_assert(KFM_HAZARD_SLOW == static_cast<int>(ObsHazardKind::Slow));
static_assert(KFM_MAX_BANANAS == kObsMaxBananas && KFM_MAX_WALLS == kObsMaxWalls);
static_assert(KFM_NO_APPROACH == kObsNoApproach &&
              KFM_APPROACH_HORIZON == kObsApproachHorizonTicks);

[[nodiscard]] Vec2 vecOf(kfm_vec v) {
  return Vec2{Fixed::fromRaw(v.x), Fixed::fromRaw(v.y)};
}

[[nodiscard]] const kfm_observation& asC(const Observation& obs) {
  // Same size, alignment and offsets (static_asserts above): one object, two views.
  return *reinterpret_cast<const kfm_observation*>(&obs);
}

std::vector<std::byte> readWasm(const std::string& dir, const std::string& name) {
  const std::string path = dir + "/" + name + ".wasm";
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

std::vector<std::byte> readExample(const std::string& name) {
  return readWasm(KFM_BOT_EXAMPLES_DIR, name);
}

// --- a chaser match -----------------------------------------------------------

constexpr std::array<uint8_t, 2> kChaserSlots = {0, 1};

struct ChaserMatch {
  int ticks{0};
  bool ended{false};
  int chaserRounds{0};
  int referenceRounds{0};
  int draws{0};
  int thrown{0};  // by the chasers
  int dashed{0};
  int caught{0};
  int perfectCatches{0};
  int referenceKilledByChasers{0};  // eliminations: victim, killer
  int chasersKilledByReference{0};
  int chasersKilledByChasers{0};
  std::array<WasmAgentStats, 2> stats{};
  std::array<AgentSlotStats, 2> slotStats{};
  double chaserSeconds{0};  // wall time inside the chasers' act()
  std::vector<uint32_t> checksums;
  std::string log;
};

[[nodiscard]] bool isChaser(uint32_t slot) {
  return slot == kChaserSlots[0] || slot == kChaserSlots[1];
}

// A WasmAgent with a stopwatch around act().
class TimedAgent final : public IAgent {
 public:
  TimedAgent(std::unique_ptr<WasmAgent> inner, double& seconds)
      : inner_(std::move(inner)), seconds_(&seconds) {}
  void init(uint64_t seed, PlayerId slot) override { inner_->init(seed, slot); }
  std::optional<PlayerInput> act(const Observation& obs, Tick tick) override {
    const auto start = std::chrono::steady_clock::now();
    std::optional<PlayerInput> in = inner_->act(obs, tick);
    *seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return in;
  }

 private:
  std::unique_ptr<WasmAgent> inner_;
  double* seconds_;
};

ChaserMatch playChaserMatch(const WasmModule& chaser, uint64_t seed, int maxTicks) {
  const ContentDB& db = defaultContent();
  const MatchContent content{db};
  WorldState s = initialWorld(MatchConfig{seed, 0b1111});
  AgentHost host;
  ChaserMatch r;
  std::array<WasmAgent*, 2> wasm{};
  for (uint8_t i = 0; i < 4; ++i) {
    const uint64_t botSeed = (seed * 31) + i;
    if (isChaser(i)) {
      auto agent = std::make_unique<WasmAgent>(chaser);
      wasm[i] = agent.get();
      host.attach(PlayerId{i}, std::make_unique<TimedAgent>(std::move(agent), r.chaserSeconds),
                  botSeed);
    } else {
      host.attach(PlayerId{i}, std::make_unique<ReferenceBot>(ReferenceBotConfig::medium()),
                  botSeed);
    }
  }
  for (const WasmAgent* a : wasm)
    REQUIRE(a->alive());
  EventSink sink;
  for (int k = 0; k < maxTicks && s.rules.phase != MatchPhase::MatchEnded; ++k) {
    InputFrame f;
    f.tick = s.tick;
    f.present = s.rules.present;
    host.step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    for (const SimEvent& e : sink) {
      const bool byChaser = e.player.isValid() && isChaser(e.player.value);
      switch (e.kind) {
        case EventKind::BoomerangThrown:
          r.thrown += byChaser ? 1 : 0;
          break;
        case EventKind::PlayerDashed:
          r.dashed += byChaser ? 1 : 0;
          break;
        case EventKind::BoomerangCaught:
          r.caught += byChaser ? 1 : 0;
          if (byChaser && e.a == static_cast<uint32_t>(CatchHow::Perfect)) ++r.perfectCatches;
          break;
        case EventKind::PlayerEliminated:
          if (e.a < kMaxPlayers) {
            const bool killerChaser = isChaser(e.a);
            if (!byChaser && killerChaser) ++r.referenceKilledByChasers;
            if (byChaser && !killerChaser) ++r.chasersKilledByReference;
            if (byChaser && killerChaser) ++r.chasersKilledByChasers;
          }
          break;
        case EventKind::RoundEnded:
          if (!e.player.isValid()) {
            ++r.draws;
          } else if (byChaser) {
            ++r.chaserRounds;
          } else {
            ++r.referenceRounds;
          }
          break;
        default:
          break;
      }
    }
    r.checksums.push_back(checksum(s));
    r.ticks = k + 1;
  }
  r.ended = s.rules.phase == MatchPhase::MatchEnded;
  for (size_t i = 0; i < wasm.size(); ++i) {
    r.stats[i] = wasm[i]->stats();
    r.slotStats[i] = host.stats(PlayerId{kChaserSlots[i]});
  }
  r.log = wasm[0]->takeLog();
  return r;
}

// --- a harvest match (v0.82) ---------------------------------------------------

// A 24 x 14 cell arena (1-unit cells, the box [-12, 12] x [-7, 7], the size
// of the contest's draft map), its own half turn: two homes (zone 0 west,
// zone 1 east), walls round the centre and in front of each home, two pits,
// six trees a side as the contest's maps have them: a poor home tree (every
// 90 ticks, one at a time), a flank tree (every 60, two) and a rich middle
// one (every 30, three). Team 0 spawns in zone 0, team 1 in zone 1. A grid harvest mode of one-tick
// turns over two teams, banking every 2 ticks, stealing every 6, to 15 banked; the same in the
// continuous space; and the grid one under the contest's fog (own eyes).
constexpr uint8_t kOrchard = 1;
constexpr uint8_t kGridHarvest = 1;
constexpr uint8_t kFreeHarvest = 2;
constexpr uint8_t kFogHarvest = 3;
constexpr int kHarvestScore = 15;

[[nodiscard]] constexpr Vec2 at2(int32_t x2, int32_t y2) {  // (x2 / 2, y2 / 2)
  return Vec2{Fixed::fromRatio(x2, 2), Fixed::fromRatio(y2, 2)};
}
[[nodiscard]] constexpr Wall wallOf(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  return Wall{Vec2{Fixed::fromInt(x0), Fixed::fromInt(y0)},
              Vec2{Fixed::fromInt(x1), Fixed::fromInt(y1)}};
}

// Home, flank and middle trees, west then east, and their beats.
constexpr std::array<Vec2, 6> kOrchardTrees = {at2(-21, -9), at2(21, 9), at2(-5, -9),
                                               at2(5, 9),    at2(-1, 5), at2(1, -5)};
constexpr std::array<uint16_t, 6> kOrchardBeats = {90, 90, 60, 60, 30, 30};
constexpr uint16_t kHomeTreeBeat = 90;

[[nodiscard]] ContentDB orchardContent() {
  ContentDB c = defaultContent();
  // The baseline: the hand raised to 3 (harvest_base's ammo_max grant).
  PowerUpDef& base = c.powerUps[c.powerUpCount];
  base = PowerUpDef{};
  base.name = ContentName::of("harvest_base");
  base.durationTicks = 1;
  base.grants[0].kind = GrantKind::Stat;
  base.grants[0].stat = StatModifier{StatKind::AmmoMax, Fixed::one(), Fixed::fromInt(2)};
  base.grantCount = 1;
  const uint8_t baseIndex = c.powerUpCount++;
  ArenaDef& a = c.arenas[kOrchard];
  a = ArenaDef{};
  a.name = ContentName::of("orchard");
  a.halfExtents = Vec2{Fixed::fromInt(12), Fixed::fromInt(7)};
  a.cellSize = Fixed::one();
  a.spawnPoints[0] = at2(-21, 1);
  a.spawnPoints[1] = at2(-21, -1);
  a.spawnPoints[2] = at2(-21, 3);
  a.spawnPoints[3] = at2(21, -1);
  a.spawnPoints[4] = at2(21, 1);
  a.spawnPoints[5] = at2(21, -3);
  a.spawnPointCount = 6;
  a.spawnGroups = 2;
  a.zones[0] = Zone{at2(-24, -2), at2(-18, 4)};
  a.zones[1] = Zone{at2(18, -4), at2(24, 2)};
  a.zoneCount = 2;
  a.walls[0] = wallOf(-1, -2, 1, 2);   // the centre block
  a.walls[1] = wallOf(-8, -4, -7, 1);  // in front of the west home
  a.walls[2] = wallOf(7, -1, 8, 4);    // in front of the east home
  a.walls[3] = wallOf(-5, 3, -4, 7);   // off the north edge
  a.walls[4] = wallOf(4, -7, 5, -3);   // off the south edge
  a.wallCount = 5;
  a.hazards[0] = Hazard{at2(-2, 6), at2(2, 10), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at2(-2, -10), at2(2, -6), HazardKind::Pit, Fixed::one()};
  a.hazardCount = 2;
  for (size_t i = 0; i < kOrchardTrees.size(); ++i) {
    PickupSpawner& t = a.pickupSpawners[a.pickupSpawnerCount++];
    t.pos = kOrchardTrees[i];
    t.intervalTicks = kOrchardBeats[i];
    t.firstTicks = t.intervalTicks;
    t.maxAlive = static_cast<uint8_t>(90 / t.intervalTicks);
    t.tree = 1;
  }
  c.arenaCount = 2;
  ModeDef& m = c.modes[kGridHarvest];
  m = ModeDef{};
  m.name = ContentName::of("harvest_1t");
  m.kind = ModeKind::Harvest;
  m.space = Space::Grid;
  m.turnTicks = 1;
  m.teams = 2;
  m.bankTicks = 2;
  m.stealTicks = 6;
  m.scoreToWin = static_cast<uint8_t>(kHarvestScore);
  m.timeLimitTicks = 36000;
  m.respawnTicks = 120;
  m.respawnInvulnTicks = 8;
  m.respawnAmmo = 0;
  m.deathBananas = DeathBananas::Drop;
  m.bananaRegrowTicks = 0;
  m.baseline = baseIndex;
  m.arena = kOrchard;
  c.modes[kFreeHarvest] = m;
  c.modes[kFreeHarvest].name = ContentName::of("harvest");
  c.modes[kFreeHarvest].space = Space::Continuous;
  c.modes[kFreeHarvest].turnTicks = 0;
  c.modes[kFogHarvest] = m;
  c.modes[kFogHarvest].name = ContentName::of("harvest_fog_1t");
  c.modes[kFogHarvest].vision = VisionDef{VisionKind::Own, 1, 4, 1, 1, 1};
  c.modeCount = 4;
  return c;
}

const ContentDB& orchard() {
  static const ContentDB c = orchardContent();
  return c;
}

struct HarvestMatch {
  int ticks{0};
  bool ended{false};
  std::array<int, 2> teamScores{};
  int banked{0};  // ScoreChanged, Objective: a banana banked
  int stolen{0};  // BananaStolen
  int laid{0};    // banana pickups laid by a tree or a drop
  int eliminations{0};
  int gathererFalls{0};  // PlayerFell of a gatherer
  // Per tree: Playing ticks a monkey of team 0 stood on its cell.
  std::array<int, kOrchardTrees.size()> team0OnTree{};
  std::vector<WasmAgentStats> stats;  // team 0's gatherers, by slot
  std::vector<AgentSlotStats> slotStats;
  std::vector<uint32_t> checksums;
};

// `perTeam` gatherers in the even slots (team 0) against team 1 in the odd
// ones: gatherers too, or medium reference bots.
HarvestMatch playHarvestMatch(const WasmModule& gatherer, uint8_t mode, bool mirror, uint64_t seed,
                              uint8_t perTeam = 2) {
  const ContentDB& db = orchard();
  REQUIRE(db.compatible(kOrchard, mode));
  const auto players = static_cast<PlayerMask>((1u << (2u * perTeam)) - 1u);
  const MatchConfig config{.seed = seed,
                           .players = players,
                           .arena = kOrchard,
                           .mode = mode,
                           .teams = teamsFor(db.modes[mode], players)};
  for (uint8_t i = 0; i < 2 * perTeam; ++i)
    REQUIRE(config.teams[i] == i % 2);
  const MatchContent content = matchContent(db, config);
  WorldState s = initialWorld(config);
  AgentHost host;
  HarvestMatch r;
  std::vector<WasmAgent*> wasm;
  for (uint8_t i = 0; i < 2 * perTeam; ++i) {
    const uint64_t botSeed = agentSeed(seed, PlayerId{i});
    if (i % 2 == 0 || mirror) {
      auto agent = std::make_unique<WasmAgent>(gatherer);
      if (i % 2 == 0) wasm.push_back(agent.get());
      host.attach(PlayerId{i}, std::move(agent), botSeed);
    } else {
      host.attach(PlayerId{i}, std::make_unique<ReferenceBot>(ReferenceBotConfig::medium()),
                  botSeed);
    }
  }
  for (const WasmAgent* a : wasm)
    REQUIRE(a->alive());
  EventSink sink;
  for (int k = 0; k < 40000 && s.rules.phase != MatchPhase::MatchEnded; ++k) {
    InputFrame f;
    f.tick = s.tick;
    f.present = s.rules.present;
    host.step(s, f, content, sink);
    REQUIRE(sink.dropped == 0);
    for (const SimEvent& e : sink) {
      if (e.kind == EventKind::ScoreChanged &&
          e.b == static_cast<uint32_t>(ScoreReason::Objective)) {
        ++r.banked;
      }
      if (e.kind == EventKind::BananaStolen) ++r.stolen;
      if (e.kind == EventKind::PickupSpawned && e.a == static_cast<uint32_t>(PickupKind::Banana)) {
        ++r.laid;
      }
      if (e.kind == EventKind::PlayerEliminated) ++r.eliminations;
      if (e.kind == EventKind::PlayerFell && e.player.isValid() &&
          (e.player.value % 2 == 0 || mirror)) {
        ++r.gathererFalls;
      }
    }
    if (s.rules.phase == MatchPhase::Playing) {
      for (uint8_t i = 0; i < 2 * perTeam; i += 2) {
        const Player* p = s.playerBySlot(PlayerId{i});
        for (size_t t = 0; p != nullptr && t < kOrchardTrees.size(); ++t)
          r.team0OnTree[t] += p->transform.pos == kOrchardTrees[t] ? 1 : 0;
      }
    }
    r.checksums.push_back(checksum(s));
    r.ticks = k + 1;
  }
  r.ended = s.rules.phase == MatchPhase::MatchEnded;
  for (uint8_t t = 0; t < 2; ++t)
    r.teamScores[t] = content.teamScore(s.rules.scores, t);
  for (size_t i = 0; i < wasm.size(); ++i) {
    r.stats.push_back(wasm[i]->stats());
    r.slotStats.push_back(host.stats(PlayerId{static_cast<uint8_t>(2 * i)}));
  }
  return r;
}

// Faults and fuel of every gatherer of team 0.
void checkGatherers(const HarvestMatch& m, uint32_t& fuelMax, uint64_t& fuel, uint64_t& acts) {
  for (size_t i = 0; i < m.stats.size(); ++i) {
    const WasmAgentStats& st = m.stats[i];
    INFO("gatherer in slot " << 2 * i);
    CHECK(st.acts + m.slotStats[i].paused == static_cast<uint64_t>(m.ticks));
    CHECK(st.traps == 0);
    CHECK(st.overBudget == 0);
    CHECK(st.badReturns == 0);
    CHECK(st.substitutions == 0);
    CHECK(st.fuelMax < 80'000u);
    fuelMax = std::max(fuelMax, st.fuelMax);
    fuel += st.fuelTotal;
    acts += st.acts;
  }
}

}  // namespace

TEST_CASE("the C SDK header mirrors Observation v2", "[ai][sdk]") {
  // The static_asserts above are the test; a few runtime reads through both
  // views of one observation make the mirror concrete.
  Observation obs{};
  obs.header.version = kObservationVersion;
  obs.header.size = sizeof(Observation);
  obs.match.inputDelayTicks = 3;
  obs.self.abilities[2].defPerfectWindowTicks = 7;
  obs.players[9].abilityPhaseTicks[2] = 11;
  obs.bananas[63].approachDistance = -5;
  obs.arena.spawners[7].maxAlive = 13;
  obs.arena.spawners[6].tree = 1;
  obs.reservedHeardCalls[15].cellY = -17;
  const kfm_observation& c = asC(obs);
  CHECK(kfm_obs_ok(&c) == 1);
  CHECK(c.match.inputDelayTicks == 3);
  CHECK(c.self.abilities[2].defPerfectWindowTicks == 7);
  CHECK(c.players[9].abilityPhaseTicks[2] == 11);
  CHECK(c.bananas[63].approachDistance == -5);
  CHECK(c.arena.spawners[7].maxAlive == 13);
  CHECK(c.arena.spawners[6].tree == 1);
  CHECK(c.arena.spawners[7].tree == 0);
  CHECK(c.heardCalls[15].cellY == -17);
  obs.self.reservedFacingOctant = 5;
  obs.match.reservedTicksToLatch = 9;
  obs.players[4].reservedVisible = 1;
  CHECK(c.self.facingOctant == 5);
  CHECK(c.match.ticksToLatch == 9);
  CHECK(c.players[4].visible == 1);
  obs.zones.count = 8;
  obs.zones.zone[7].maxY = -19;
  CHECK(c.zones.count == 8);
  CHECK(c.zones.zone[7].maxY == -19);
  // A version 1 observation is not one this header reads.
  obs.header.version = kObservationV1Version;
  obs.header.size = kObservationV1Size;
  CHECK(kfm_obs_ok(&c) == 0);
  CHECK(kfm_pack_input(1, 2, 3, 4, 5) == packInput(PlayerInput{1, 2, 3, 4, 5}));
}

TEST_CASE("the C SDK's integer math is the engine's", "[ai][sdk]") {
  for (uint32_t bam = 0; bam < 65536; ++bam) {
    const Angle a = Angle::fromBam(static_cast<uint16_t>(bam));
    INFO("bam " << bam);
    REQUIRE(kfm_sin(static_cast<uint16_t>(bam)) == sin(a).raw);
    REQUIRE(kfm_cos(static_cast<uint16_t>(bam)) == cos(a).raw);
  }
  Rng engine = Rng::seeded(0xC0FFEEull);
  Rng draws = Rng::seeded(99);
  kfm_rng sdk{};
  kfm_rng_seed(&sdk, 0xC0FFEEull);
  for (int i = 0; i < 20000; ++i) {
    // Vectors of every scale, both signs and the axes.
    const int32_t shift = draws.range(0, 26);
    const int32_t x = draws.range(-(1 << shift), 1 << shift) * (i % 7 == 0 ? 0 : 1);
    const int32_t y = draws.range(-(1 << shift), 1 << shift);
    const Vec2 v{Fixed::fromRaw(x), Fixed::fromRaw(y)};
    INFO("v " << x << ", " << y);
    REQUIRE(kfm_angle_of(kfm_v(x, y)) == angleOf(v).bam);
    REQUIRE(kfm_angle8_of(kfm_v(x, y)) == kfm_bam_to_angle8(angleOf(v).bam));
    REQUIRE(vecOf(kfm_vec_normalized(kfm_v(x, y))) == normalized(v));
    REQUIRE(kfm_vec_len(kfm_v(x, y)) == length(v).raw);
    REQUIRE(kfm_fx_mul(x, y) == (Fixed::fromRaw(x) * Fixed::fromRaw(y)).raw);
    if (y != 0) REQUIRE(kfm_fx_div(x, y) == (Fixed::fromRaw(x) / Fixed::fromRaw(y)).raw);
    if (x >= 0) REQUIRE(kfm_fx_sqrt(x) == sqrt(Fixed::fromRaw(x)).raw);
    const auto heading = static_cast<uint16_t>(draws.next());
    REQUIRE(vecOf(kfm_vec_from_bam(heading, y)) ==
            Vec2::fromAngle(Angle::fromBam(heading), Fixed::fromRaw(y)));
    REQUIRE(kfm_bam_delta(heading, static_cast<uint16_t>(x)) ==
            Angle::fromBam(heading).signedDeltaFrom(Angle::fromBam(static_cast<uint16_t>(x))));
    REQUIRE(kfm_rng_next(&sdk) == engine.next());
    REQUIRE(kfm_rng_below(&sdk, 1000) == engine.below(1000));
    REQUIRE(kfm_rng_range(&sdk, -7, 9) == engine.range(-7, 9));
  }
}

TEST_CASE("the C SDK's flight step is the sim's ProjectileSystem", "[ai][sdk]") {
  // Four reference bots on the dojo with four interior walls (so bananas
  // bounce off walls, corners and the outer box). Each tick, every banana in
  // flight before and after the step (ticksAlive grew by one: the same
  // banana, moved this tick) is stepped by kfm_flight_step from the
  // observation before, with its owner's position after the step, and must
  // land exactly where the sim put it, with the same heading and phase.
  ContentDB db = defaultContent();
  ArenaDef& arena = db.arenas[0];
  const auto box = [](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    return Wall{Vec2{Fixed::fromInt(x0), Fixed::fromInt(y0)},
                Vec2{Fixed::fromInt(x1), Fixed::fromInt(y1)}};
  };
  arena.walls[0] = box(-2, 2, 2, 3);
  arena.walls[1] = box(-2, -3, 2, -2);
  arena.walls[2] = box(-7, -1, -6, 1);
  arena.walls[3] = box(6, -1, 7, 1);
  arena.wallCount = 4;
  const MatchContent content{db};
  for (const uint64_t seed : {5u, 6u}) {
    WorldState s = initialWorld(MatchConfig{seed, 0b1111});
    AgentHost host;
    for (uint8_t i = 0; i < 4; ++i) {
      host.attach(PlayerId{i}, std::make_unique<ReferenceBot>(), (seed * 17) + i);
    }
    Observation before{};
    Observation after{};
    EventSink sink;
    int compared = 0;
    int returning = 0;
    int bounced = 0;
    for (int k = 0; k < 12000 && s.rules.phase != MatchPhase::MatchEnded; ++k) {
      buildObservation(s, content, PlayerId{0}, 0, before);
      InputFrame f;
      f.tick = s.tick;
      f.present = s.rules.present;
      host.step(s, f, content, sink);
      buildObservation(s, content, PlayerId{0}, 0, after);
      for (uint32_t i = 0; i < kObsMaxBananas; ++i) {
        const ObsBanana& b0 = before.bananas[i];
        const ObsBanana& b1 = after.bananas[i];
        if (b0.valid == 0 || b1.valid == 0 || b0.phase == KFM_BANANA_GROUNDED ||
            b1.ticksAlive != b0.ticksAlive + 1 || b0.owner >= kObsMaxPlayers ||
            (before.players[b0.owner].alive != 0 && after.players[b0.owner].alive == 0)) {
          continue;
        }
        const ObsPlayer& owner = after.players[b0.owner];
        kfm_flight fl = kfm_flight_of(&asC(before).bananas[i]);
        const int bounce = kfm_flight_step(&fl, &asC(before), kfm_v(owner.posX, owner.posY),
                                           before.players[b0.owner].alive);
        INFO("seed " << seed << " tick " << before.match.tick << " banana " << i << " phase "
                     << int{b0.phase} << " -> " << int{b1.phase});
        REQUIRE(fl.pos.x == b1.posX);
        REQUIRE(fl.pos.y == b1.posY);
        REQUIRE(fl.vel.x == b1.velX);
        REQUIRE(fl.vel.y == b1.velY);
        REQUIRE(fl.heading == b1.headingBam);
        REQUIRE(fl.phase == b1.phase);
        REQUIRE(fl.ticksInPhase == b1.ticksInPhase);
        ++compared;
        returning += b0.phase == KFM_BANANA_RETURNING ? 1 : 0;
        bounced += bounce;
      }
    }
    UNSCOPED_INFO("seed " << seed << ": " << compared << " steps, " << returning << " returning, "
                          << bounced << " bounces");
    CHECK(compared > 1000);
    CHECK(returning > 100);
    CHECK(bounced > 10);
  }
}

TEST_CASE("the C SDK's example bots load", "[ai][sdk][wasm]") {
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  // Loading checks the four exports, the imports, the memory and, on a probe
  // instance, that kfm_abi_version() is a version the host speaks (the
  // examples are built against the header: version 2).
  auto idle = WasmModule::load(readExample("idle"));
  if (!idle) FAIL("idle: " << idle.error().message);
  if (!idle) return;
  CHECK_FALSE(idle->importsLog());
  auto chaser = WasmModule::load(readExample("chaser"));
  if (!chaser) FAIL("chaser: " << chaser.error().message);
  if (!chaser) return;
  CHECK(chaser->importsLog());
  auto gatherer = WasmModule::load(readExample("gatherer"));
  if (!gatherer) FAIL("gatherer: " << gatherer.error().message);
  if (!gatherer) return;
  CHECK(gatherer->importsLog());
  {
    WasmAgent g{*gatherer};
    g.init(7, PlayerId{2});
    REQUIRE(g.alive());
    CHECK(g.takeLog() == "gatherer: slot 2 ready\n");
    CHECK(g.act(Observation{}, Tick{0}).has_value());
    CHECK(g.stats().traps == 0);
  }

  WasmAgent agent{*chaser};
  agent.init(42, PlayerId{3});
  REQUIRE(agent.alive());
  CHECK(agent.takeLog() == "chaser: slot 3 ready\n");
  // An observation the header refuses (version 0): neutral input, no fault.
  const Observation empty{};
  const auto in = agent.act(empty, Tick{0});
  CHECK(in.has_value());
  CHECK(agent.stats().traps == 0);
}

TEST_CASE("the chaser plays matches against the reference bot", "[ai][sdk][wasm]") {
  // Two chasers (slots 0, 1) against two medium reference bots (2, 3) on the
  // default content, free for all: every match within budget, never a fault,
  // with throws, dashes and catches. Who wins is reported, not asserted.
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  auto chaser = WasmModule::load(readExample("chaser"));
  if (!chaser) FAIL("chaser: " << chaser.error().message);
  if (!chaser) return;

  constexpr int kMaxTicks = 36000;
  ChaserMatch total;
  uint64_t acts = 0;
  uint64_t fuel = 0;
  uint32_t fuelMax = 0;
  uint32_t fuelInit = 0;
  int ticks = 0;
  int ended = 0;
  std::vector<uint32_t> firstChecksums;
  for (const uint64_t seed : {2026u, 2027u, 2028u, 2029u}) {
    const ChaserMatch m = playChaserMatch(*chaser, seed, kMaxTicks);
    INFO("seed " << seed);
    for (size_t i = 0; i < m.stats.size(); ++i) {
      const WasmAgentStats& st = m.stats[i];
      INFO("chaser in slot " << int{kChaserSlots[i]});
      // Every tick, but those it sat out eliminated (v0.35, the lifecycle).
      CHECK(st.acts + m.slotStats[i].paused == static_cast<uint64_t>(m.ticks));
      CHECK(st.traps == 0);
      CHECK(st.overBudget == 0);
      CHECK(st.badReturns == 0);
      CHECK(st.substitutions == 0);
      CHECK(m.slotStats[i].substitutions == 0);
      CHECK(st.fuelMax < 100'000u);
      acts += st.acts;
      fuel += st.fuelTotal;
      fuelMax = std::max(fuelMax, st.fuelMax);
      fuelInit = std::max(fuelInit, st.fuelInit);
    }
    CHECK(m.log == "chaser: slot 0 ready\n");
    CHECK(m.thrown > 0);
    CHECK(m.dashed > 0);
    CHECK(m.caught > 0);
    if (firstChecksums.empty()) firstChecksums = m.checksums;
    ticks += m.ticks;
    ended += m.ended ? 1 : 0;
    total.chaserRounds += m.chaserRounds;
    total.referenceRounds += m.referenceRounds;
    total.draws += m.draws;
    total.referenceKilledByChasers += m.referenceKilledByChasers;
    total.chasersKilledByReference += m.chasersKilledByReference;
    total.chasersKilledByChasers += m.chasersKilledByChasers;
    total.thrown += m.thrown;
    total.dashed += m.dashed;
    total.caught += m.caught;
    total.perfectCatches += m.perfectCatches;
    total.chaserSeconds += m.chaserSeconds;
  }
  WARN("4 matches, chasers vs reference (medium): "
       << ended << " ended in " << ticks << " ticks; rounds chasers " << total.chaserRounds
       << ", reference " << total.referenceRounds << ", draws " << total.draws
       << "; eliminations: reference by chasers " << total.referenceKilledByChasers
       << ", chasers by reference " << total.chasersKilledByReference << ", chasers by chasers "
       << total.chasersKilledByChasers);
  WARN("chaser events: " << total.thrown << " throws, " << total.dashed << " dashes, "
                         << total.caught << " catches (" << total.perfectCatches << " perfect)");
  WARN("chaser fuel per act: mean "
       << (acts == 0 ? 0 : fuel / acts) << ", max " << fuelMax << " (budget 100000), kfm_init "
       << fuelInit << "; wall "
       << total.chaserSeconds * 1e6 / static_cast<double>(std::max<uint64_t>(acts, 1))
       << " us per act");

  // Deterministic: the first seed replays the same match, checksum for checksum.
  const ChaserMatch again = playChaserMatch(*chaser, 2026, kMaxTicks);
  CHECK(again.checksums == firstChecksums);
}

TEST_CASE("the chaser built against version 1 plays as the version 2 build (v0.80)",
          "[ai][sdk][wasm]") {
  // tests/fixtures/bots/chaser_v1.wasm is the chaser.wasm checked in before
  // v0.80, byte for byte: chaser.c built against the version 1 header. Its
  // kfm_obs_ok wants version 1 and 5748 bytes, and the chaser reads nothing
  // version 2 added, so when the host serves it version 1's prefix it plays
  // exactly what the version 2 build plays.
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  auto v1 = WasmModule::load(readWasm(KFM_BOT_FIXTURES_DIR, "chaser_v1"));
  if (!v1) FAIL("chaser_v1: " << v1.error().message);
  if (!v1) return;
  auto v2 = WasmModule::load(readExample("chaser"));
  if (!v2) FAIL("chaser: " << v2.error().message);
  if (!v2) return;
  constexpr int kMaxTicks = 36000;
  const ChaserMatch old = playChaserMatch(*v1, 2027, kMaxTicks);
  const ChaserMatch now = playChaserMatch(*v2, 2027, kMaxTicks);
  CHECK(old.ended);
  CHECK(old.thrown > 0);
  CHECK(old.caught > 0);
  for (const WasmAgentStats& st : old.stats) {
    CHECK(st.traps == 0);
    CHECK(st.substitutions == 0);
  }
  CHECK(old.log == now.log);
  CHECK(old.checksums == now.checksums);
}

TEST_CASE("the gatherer banks a harvest match to its score (v0.82)", "[ai][sdk][wasm][harvest]") {
  // The kind's smoke test (the reference bot never banks): on the orchard,
  // in the grid of one-tick turns, in the continuous space and in the grid
  // under fog, two gatherers against two gatherers and against two medium
  // reference bots. Every match ends by score_to_win, the score being banked
  // bananas less stolen ones, with no fault and the fuel well under the
  // budget; no gatherer walks into a pit when nobody fights; against the
  // reference bots the gatherers' team is the one that gets there.
  // Deterministic, checksum for checksum.
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  auto gatherer = WasmModule::load(readExample("gatherer"));
  if (!gatherer) FAIL("gatherer: " << gatherer.error().message);
  if (!gatherer) return;
  uint32_t fuelMax = 0;
  uint64_t fuel = 0;
  uint64_t acts = 0;
  std::vector<uint32_t> first;
  for (const uint8_t mode : {kGridHarvest, kFreeHarvest, kFogHarvest}) {
    for (const bool mirror : {true, false}) {
      for (const uint64_t seed : {81u, 82u}) {
        const HarvestMatch m = playHarvestMatch(*gatherer, mode, mirror, seed);
        INFO("mode " << int{mode} << (mirror ? " mirror" : " vs reference") << " seed " << seed);
        WARN("mode " << int{mode} << (mirror ? " mirror" : " vs reference") << " seed " << seed
                     << ": " << m.ticks << " ticks, " << m.teamScores[0] << " - " << m.teamScores[1]
                     << ", " << m.banked << " banked, " << m.stolen << " stolen, " << m.laid
                     << " laid, " << m.eliminations << " eliminations, " << m.gathererFalls
                     << " gatherer falls");
        CHECK(m.ended);
        CHECK(std::max(m.teamScores[0], m.teamScores[1]) >= kHarvestScore);
        CHECK(m.banked - m.stolen == m.teamScores[0] + m.teamScores[1]);
        if (!mirror) CHECK(m.teamScores[0] >= kHarvestScore);
        if (mirror) CHECK(m.gathererFalls == 0);
        checkGatherers(m, fuelMax, fuel, acts);
        if (first.empty()) first = m.checksums;
      }
    }
  }
  WARN("gatherer fuel per act: mean " << (acts == 0 ? 0 : fuel / acts) << ", max " << fuelMax
                                      << " (budget 100000)");
  const HarvestMatch again = playHarvestMatch(*gatherer, kGridHarvest, true, 81);
  CHECK(again.checksums == first);
}

TEST_CASE("a team of gatherers spreads over its trees under fog (v0.82)",
          "[ai][sdk][wasm][harvest]") {
  // Three gatherers a side under the contest's fog, where a monkey sees no
  // fruit from its home: each works the tree its rank gives it (the rich
  // middle tree, the flank tree, the rich one again by the team's share-out),
  // so team 0 stands for a long while on more than one tree, and a team
  // banks faster than its home tree alone could feed it (one banana a beat).
  const WasmRuntime runtime;
  REQUIRE(runtime.ok());
  auto gatherer = WasmModule::load(readExample("gatherer"));
  if (!gatherer) FAIL("gatherer: " << gatherer.error().message);
  if (!gatherer) return;
  uint32_t fuelMax = 0;
  uint64_t fuel = 0;
  uint64_t acts = 0;
  for (const bool mirror : {true, false}) {
    const HarvestMatch m = playHarvestMatch(*gatherer, kFogHarvest, mirror, 83, 3);
    INFO((mirror ? "mirror" : "vs reference"));
    // A tree is worked when a monkey of team 0 stands on it for a fifth of
    // the match or more. (A fixed second was too near the flank trees'
    // share once a fruit two monkeys touch at once went to a drawn one, no
    // longer always to the lower slot: 57 ticks each of a 222-tick match.)
    int worked = 0;
    for (size_t t = 0; t < kOrchardTrees.size(); ++t)
      worked += m.team0OnTree[t] * 5 >= m.ticks ? 1 : 0;
    WARN((mirror ? "mirror" : "vs reference")
         << ": " << m.ticks << " ticks, " << m.teamScores[0] << " - " << m.teamScores[1]
         << ", team 0 on the trees " << m.team0OnTree[0] << " " << m.team0OnTree[1] << " "
         << m.team0OnTree[2] << " " << m.team0OnTree[3] << " " << m.team0OnTree[4] << " "
         << m.team0OnTree[5] << " ticks, " << worked << " trees worked");
    // The match ends by score: against the reference bots team 0's; in the
    // mirror whichever team's (the world Rng draws its tied cells and
    // fruit, v0.82, so the winner is the seed's), and the winner banks
    // faster than its home tree's beat: more than one banana a beat.
    const int winner = std::max(m.teamScores[0], m.teamScores[1]);
    CHECK(m.ended);
    CHECK(winner >= kHarvestScore);
    if (!mirror) CHECK(m.teamScores[0] >= kHarvestScore);
    CHECK(worked >= 2);
    CHECK(winner * kHomeTreeBeat > m.ticks);
    checkGatherers(m, fuelMax, fuel, acts);
  }
  WARN("gatherer fuel per act: mean " << (acts == 0 ? 0 : fuel / acts) << ", max " << fuelMax);
}
