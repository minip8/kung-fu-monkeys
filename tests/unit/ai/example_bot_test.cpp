// The C SDK seed (tools/bots/sdk-c/kfm_bot.h) and its example bots
// (tools/bots/examples, architecture §8): the header's Observation layout and
// constants are the C++ ones, its integer math is the engine's bit for bit,
// its banana flight step reproduces the sim's ProjectileSystem exactly, and
// the checked-in chaser.wasm, built from the header, loads and plays a whole
// match against the reference bot within budget, deterministically. The win
// counts and the fuel and wall numbers are printed, never asserted.
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
KFM_SAME_AT(kfm_obs_arena, spawnerCount, ObsArena);
KFM_SAME_AT(kfm_obs_arena, spawners, ObsArena);
KFM_SAME_AT(kfm_obs_heard_call, cellY, ObsHeardCall);
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

std::vector<std::byte> readExample(const std::string& name) {
  const std::string path = std::string{KFM_BOT_EXAMPLES_DIR} + "/" + name + ".wasm";
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
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
    host.fill(f, s, content);
    sink.clear();
    step(s, f, content, sink);
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

}  // namespace

TEST_CASE("the C SDK header mirrors Observation v1", "[ai][sdk]") {
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
  obs.reservedHeardCalls[15].cellY = -17;
  const kfm_observation& c = asC(obs);
  CHECK(kfm_obs_ok(&c) == 1);
  CHECK(c.match.inputDelayTicks == 3);
  CHECK(c.self.abilities[2].defPerfectWindowTicks == 7);
  CHECK(c.players[9].abilityPhaseTicks[2] == 11);
  CHECK(c.bananas[63].approachDistance == -5);
  CHECK(c.arena.spawners[7].maxAlive == 13);
  CHECK(c.heardCalls[15].cellY == -17);
  obs.self.reservedFacingOctant = 5;
  obs.match.reservedTicksToLatch = 9;
  obs.players[4].reservedVisible = 1;
  CHECK(c.self.facingOctant == 5);
  CHECK(c.match.ticksToLatch == 9);
  CHECK(c.players[4].visible == 1);
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
      host.fill(f, s, content);
      sink.clear();
      step(s, f, content, sink);
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
  // instance, that kfm_abi_version() is 1 (AbiMismatch otherwise).
  auto idle = WasmModule::load(readExample("idle"));
  if (!idle) FAIL("idle: " << idle.error().message);
  if (!idle) return;
  CHECK_FALSE(idle->importsLog());
  auto chaser = WasmModule::load(readExample("chaser"));
  if (!chaser) FAIL("chaser: " << chaser.error().message);
  if (!chaser) return;
  CHECK(chaser->importsLog());

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
