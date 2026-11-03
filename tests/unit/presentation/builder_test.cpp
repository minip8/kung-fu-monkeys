// RenderState derivation (presentation/builder.hpp) over real sim states
// driven through step(): interpolation and snapping, correction smoothing,
// cosmetic height curves, facing, event-driven effects, the HUD, and a
// scripted 10-player match end to end.
#include "kfm/presentation/builder.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/presentation/world_basis.hpp"
#include "kfm/sim/scripted_inputs.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <vector>

using namespace kfm;
using namespace kfm::presentation;
using namespace kfm::sim;
using namespace kfm::sim::test;
using Catch::Approx;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr uint8_t kDashBit = buttonBit(Button::Dash);
constexpr uint8_t kThrowBit = buttonBit(Button::Throw);
const PlayerId kP0{0};
const PlayerId kP1{1};

// Steps one tick with player 0's input; returns the tick's events.
EventLog stepWith(WorldState& s, PlayerInput in0, PlayerInput in1 = {}) {
  InputFrame f = frameFor(s);
  f.inputs[0] = in0;
  f.inputs[1] = in1;
  EventLog log;
  runTicks(s, f, 1, defaultContent(), log);
  return log;
}

RenderState build(PresentationState& ps, const WorldState& prev, const WorldState& curr,
                  float alpha, std::span<const SimEvent> events = {}, float dt = kDt) {
  return buildRenderState(ps, prev, curr, alpha, events, defaultContent(), dt);
}

const RenderPlayer* findPlayer(const RenderState& rs, uint8_t slot) {
  for (uint16_t i = 0; i < rs.playerCount; ++i) {
    if (rs.players[i].slot == slot) return &rs.players[i];
  }
  return nullptr;
}

const RenderPlayer& playerIn(const RenderState& rs, uint8_t slot) {
  const RenderPlayer* p = findPlayer(rs, slot);
  if (p == nullptr) {
    FAIL_CHECK("no render player in slot " << int{slot});
    std::abort();
  }
  return *p;
}

int countEffects(const RenderState& rs, EffectKind kind) {
  int n = 0;
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    if (rs.effects[i].kind == kind) ++n;
  }
  return n;
}

Vec2f groundOf(const RenderPlayer& p) {
  return worldToSim(p.ground);
}
Vec2f drawnOf(const RenderPlayer& p) {
  return worldToSim(p.position);
}

}  // namespace

TEST_CASE("positions interpolate by alpha; alpha 1 renders the newest tick",
          "[presentation][builder]") {
  WorldState prev = makePlayingWorld(11, 0b11);
  placePlayer(prev, kP0, Vec2{Fixed::fromInt(-4), Fixed::zero()});
  // Walk +x for a few ticks so prev -> curr is a real one-tick move.
  for (int i = 0; i < 5; ++i)
    stepWith(prev, PlayerInput{0, 255, 0, 0, 0});
  WorldState curr = prev;
  stepWith(curr, PlayerInput{0, 255, 0, 0, 0});
  const float x0 = playerAt(prev, kP0).transform.pos.x.toFloat();
  const float x1 = playerAt(curr, kP0).transform.pos.x.toFloat();
  REQUIRE(x1 > x0);

  for (const float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
    PresentationState ps;
    const RenderState rs = build(ps, prev, curr, alpha);
    const RenderPlayer& p = playerIn(rs, 0);
    CHECK(p.position.x == Approx(lerp(x0, x1, alpha)));
    CHECK(p.ground.x == Approx(lerp(x0, x1, alpha)));
    CHECK(p.ground.y == 0.0f);
  }
  // The same state twice renders it as is.
  PresentationState ps;
  const RenderState rs = build(ps, curr, curr, 0.3f);
  CHECK(playerIn(rs, 0).position.x == Approx(x1));
  CHECK(rs.playerCount == 2);
  CHECK(rs.shadowCount == 2);
}

TEST_CASE("teleports and new entities snap instead of sliding", "[presentation][builder]") {
  WorldState prev = makePlayingWorld(12, 0b11);
  placePlayer(prev, kP0, Vec2{Fixed::fromInt(-10), Fixed::zero()});
  WorldState curr = prev;
  placePlayer(curr, kP0, Vec2{Fixed::fromInt(10), Fixed::fromInt(3)});  // > kSnapDistance
  PresentationState ps;
  RenderState rs = build(ps, prev, curr, 0.5f);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(10.0f));
  CHECK(drawnOf(playerIn(rs, 0)).y == Approx(3.0f));

  // Same slot, new generation: a different entity, also snapped.
  WorldState respawned = prev;
  respawned.players.despawn(respawned.players.idAt(0));
  REQUIRE(respawned.players.spawnAt(0).isValid());
  respawned.players.items[0] = playerAt(prev, kP0);
  respawned.players.items[0].transform.pos = Vec2{Fixed::fromInt(-9), Fixed::zero()};
  PresentationState ps2;
  rs = build(ps2, prev, respawned, 0.5f);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(-9.0f));

  // A banana only in curr (just thrown) is drawn at its curr position.
  WorldState withBanana = prev;
  const EntityId b = spawnBanana(withBanana, kP1, Vec2{Fixed::fromInt(2), Fixed::fromInt(2)},
                                 Vec2{Fixed::fromRatio(1, 4), Fixed::zero()});
  PresentationState ps3;
  rs = build(ps3, prev, withBanana, 0.0f);
  REQUIRE(rs.bananaCount == 1);
  CHECK(worldToSim(rs.bananas[0].ground).x == Approx(2.0f));
  CHECK(rs.bananas[0].ownerSlot == 1);
  CHECK(rs.shadowCount == 3);
  CHECK(b.isValid());
}

TEST_CASE("rollback corrections are smoothed and decay exponentially", "[presentation][builder]") {
  WorldState s = makePlayingWorld(13, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::zero(), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(5), Fixed::zero()});
  PresentationState ps;
  RenderState rs = build(ps, s, s, 1.0f);
  REQUIRE(drawnOf(playerIn(rs, 0)).x == Approx(0.0f));

  // The corrected timeline puts both players elsewhere: P0 by a small error,
  // P1 by a large one.
  WorldState corrected = s;
  placePlayer(corrected, kP0, Vec2{Fixed::half(), Fixed::zero()});
  placePlayer(corrected, kP1, Vec2{Fixed::fromInt(9), Fixed::zero()});
  onRollback(ps, corrected, corrected, 1.0f);
  CHECK(ps.players[0].motion.visualOffset.x == Approx(-0.5f));
  CHECK(ps.players[1].motion.visualOffset == Vec2f{});  // above the snap threshold

  // dt = 0: the model has not moved yet; the shadow is already on the truth.
  rs = build(ps, corrected, corrected, 1.0f, {}, 0.0f);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(0.0f));
  CHECK(groundOf(playerIn(rs, 0)).x == Approx(0.5f));
  CHECK(drawnOf(playerIn(rs, 1)).x == Approx(9.0f));

  // One time constant later the error has decayed to 1/e.
  rs = build(ps, corrected, corrected, 1.0f, {}, kSmoothingTau);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(0.5f - (0.5f * std::exp(-1.0f))).margin(1e-4));
  // And it is gone after many.
  for (int i = 0; i < 60; ++i)
    rs = build(ps, corrected, corrected, 1.0f);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(0.5f));
  CHECK(ps.players[0].motion.visualOffset == Vec2f{});

  // Local players blend much faster.
  placePlayer(corrected, kP0, Vec2{Fixed::one(), Fixed::zero()});
  onRollback(ps, corrected, corrected, 1.0f, PlayerMask{0b01});
  rs = build(ps, corrected, corrected, 1.0f, {}, kLocalSmoothingTau);
  CHECK(drawnOf(playerIn(rs, 0)).x == Approx(1.0f - (0.5f * std::exp(-1.0f))).margin(1e-4));
}

TEST_CASE("dash hop is zero at take-off and landing and positive in between",
          "[presentation][builder]") {
  const ContentDB& content = defaultContent();
  const AbilityDef& dash = content.abilityForSlot(kAbilitySlotDash);
  WorldState s = makePlayingWorld(14, 0b1);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-8), Fixed::zero()});
  CHECK(dashHopHeight(playerAt(s, kP0), content) == 0.0f);

  stepWith(s, PlayerInput{0, 0, 0, kDashBit, 0});
  std::vector<float> heights;
  heights.push_back(dashHopHeight(playerAt(s, kP0), content));
  for (int k = 1; k < dash.activeTicks + 2; ++k) {
    stepWith(s, PlayerInput{});
    heights.push_back(dashHopHeight(playerAt(s, kP0), content));
  }
  for (int k = 0; k < int{dash.activeTicks}; ++k) {
    INFO("active tick " << k);
    CHECK(heights[static_cast<std::size_t>(k)] > 0.0f);
    CHECK(heights[static_cast<std::size_t>(k)] <= kDashHopHeight);
  }
  CHECK(heights[dash.activeTicks] == 0.0f);  // recovery: landed
  // Symmetric arc: highest in the middle.
  CHECK(heights[dash.activeTicks / 2] > heights[0]);
  CHECK(heights[dash.activeTicks / 2] > heights[dash.activeTicks - 1]);
}

TEST_CASE("hit-stun lift is positive only during hit-stun", "[presentation][builder]") {
  const ContentDB& content = defaultContent();
  Player p;
  CHECK(hitLiftHeight(p, content) == 0.0f);
  for (uint16_t left = 1; left <= content.player.hitStunTicks; ++left) {
    p.hitStunTicks = left;
    CHECK(hitLiftHeight(p, content) > 0.0f);
    CHECK(hitLiftHeight(p, content) <= kHitLiftHeight);
  }
  p.hitStunTicks = 0;
  CHECK(playerHeight(p, content) == 0.0f);
}

TEST_CASE("banana arc is zero at the throw and back at the owner", "[presentation][builder]") {
  const ContentDB& content = defaultContent();
  Boomerang b;
  b.phase = BoomerangPhase::Outbound;
  b.ticksInPhase = 0;
  CHECK(bananaArcHeight(b, 0.0f, -1.0f, content) == 0.0f);
  b.ticksInPhase = content.boomerang.outboundTicks / 2;
  CHECK(bananaArcHeight(b, 0.0f, -1.0f, content) > 0.0f);
  b.ticksInPhase = content.boomerang.outboundTicks;
  CHECK(bananaArcHeight(b, 0.0f, -1.0f, content) == Approx(kBananaArcPeak));
  b.phase = BoomerangPhase::Returning;
  CHECK(bananaArcHeight(b, 6.0f, 6.0f, content) == Approx(kBananaArcPeak));
  CHECK(bananaArcHeight(b, 3.0f, 6.0f, content) > 0.0f);
  CHECK(bananaArcHeight(b, 0.0f, 6.0f, content) == 0.0f);
  b.phase = BoomerangPhase::Grounded;
  CHECK(bananaArcHeight(b, 3.0f, 6.0f, content) == 0.0f);

  // A real throw: the banana is airborne for its whole flight and its shadow
  // stays on the sim position.
  WorldState s = makePlayingWorld(15, 0b1);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-6), Fixed::zero()});
  PresentationState ps;
  WorldState prev = s;
  EventLog log = stepWith(s, PlayerInput{0, 0, 0, kThrowBit, 0});
  int airborneFrames = 0;
  float peak = 0.0f;
  for (int k = 0; k < 200; ++k) {
    const RenderState rs = build(ps, prev, s, 1.0f, log);
    for (uint16_t i = 0; i < rs.bananaCount; ++i) {
      const RenderBanana& rb = rs.bananas[i];
      CHECK(rb.height >= kBananaHandHeight);
      CHECK(rb.position.y == rb.height);
      CHECK(rb.ground.y == 0.0f);
      CHECK(rb.ground.x == rb.position.x);
      peak = std::max(peak, rb.height - kBananaHandHeight);
      ++airborneFrames;
    }
    prev = s;
    log = stepWith(s, PlayerInput{});
    if (countEvents(log, EventKind::BoomerangCaught) > 0) break;
  }
  CHECK(airborneFrames > content.boomerang.outboundTicks);
  CHECK(peak == Approx(kBananaArcPeak).margin(0.05));
}

TEST_CASE("body yaw turns at a limited rate; aim yaw follows directly", "[presentation][builder]") {
  WorldState s = makePlayingWorld(16, 0b1);
  placePlayer(s, kP0, Vec2{});
  stepWith(s, PlayerInput{0, 255, 0, 0, 0});  // move and aim along +x
  PresentationState ps;
  RenderState rs = build(ps, s, s, 1.0f);
  CHECK(playerIn(rs, 0).bodyYaw == Approx(0.0f).margin(1e-5));

  // Stick to -x (half a turn), aim to +y (a quarter turn).
  WorldState prev = s;
  stepWith(s, PlayerInput{128, 255, 64, 0, 0});
  rs = build(ps, prev, s, 1.0f);
  CHECK(playerIn(rs, 0).aimYaw == Approx(0.5f * kPi));
  CHECK(std::fabs(playerIn(rs, 0).bodyYaw) == Approx(kBodyTurnRate * kDt).margin(1e-4));

  int frames = 1;
  while (std::fabs(std::fabs(playerIn(rs, 0).bodyYaw) - kPi) > 1e-4f && frames < 100) {
    const float before = playerIn(rs, 0).bodyYaw;
    rs = build(ps, s, s, 1.0f);
    CHECK(std::fabs(wrapRadians(playerIn(rs, 0).bodyYaw - before)) <=
          (kBodyTurnRate * kDt) + 1e-5f);
    ++frames;
  }
  // Half a turn at 720 deg/s takes 0.25 s = 15 frames (16 if rounding leaves a sliver).
  CHECK(frames >= 15);
  CHECK(frames <= 16);

  // Releasing the stick keeps the last direction.
  prev = s;
  stepWith(s, PlayerInput{0, 0, 64, 0, 0});
  rs = build(ps, prev, s, 1.0f);
  CHECK(std::fabs(playerIn(rs, 0).bodyYaw) == Approx(kPi).margin(1e-4));
}

TEST_CASE("events start effects whose timers count down", "[presentation][builder]") {
  WorldState s = makePlayingWorld(17, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  PresentationState ps;
  build(ps, s, s, 1.0f);

  const EntityId p0 = s.players.idAt(0);
  const EntityId p1 = s.players.idAt(1);
  const std::vector<SimEvent> events = {
      SimEvent{EventKind::PlayerDashed, s.tick, kP0, p0, 0, 0},
      SimEvent{EventKind::BoomerangCaught, s.tick, kP1, EntityId{},
               static_cast<uint32_t>(CatchHow::Perfect), 0},
      SimEvent{EventKind::PlayerHit, s.tick, kP1, EntityId{}, 0, 0},
      SimEvent{EventKind::BoomerangThrown, s.tick, kP0, EntityId{}, 0, 0},
      SimEvent{EventKind::PlayerEliminated, s.tick, kP1, p1, 0xFF, 0},
      SimEvent{EventKind::RoundEnded, s.tick, kP0, EntityId{}, 1, 0},
      SimEvent{EventKind::BoomerangGrounded, s.tick, PlayerId::invalid(), EntityId{}, 0, 0},
  };
  RenderState rs = build(ps, s, s, 1.0f, events);
  CHECK(rs.effectCount == 6);  // BoomerangGrounded has no effect
  CHECK(countEffects(rs, EffectKind::DashRing) == 1);
  CHECK(countEffects(rs, EffectKind::PerfectCatchFlash) == 1);
  CHECK(countEffects(rs, EffectKind::HitFlash) == 1);
  CHECK(countEffects(rs, EffectKind::ThrowPuff) == 1);
  CHECK(countEffects(rs, EffectKind::EliminationBurst) == 1);
  CHECK(countEffects(rs, EffectKind::RoundEndBanner) == 1);
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    const RenderEffect& e = rs.effects[i];
    CHECK(e.remaining == e.duration);
    CHECK(e.progress() == 0.0f);
    if (e.kind == EffectKind::DashRing) {
      CHECK(e.slot == 0);
      CHECK(worldToSim(e.position).x == Approx(-3.0f));
      CHECK(e.position.y == kEffectGroundHeight);
    }
    if (e.kind == EffectKind::EliminationBurst) {
      CHECK(e.slot == 1);
      CHECK(worldToSim(e.position).x == Approx(3.0f));
    }
    if (e.kind == EffectKind::RoundEndBanner) CHECK(e.slot == 0);
  }
  CHECK(rs.shakeAmplitude ==
        Approx(kShakeOnHit + kShakeOnElimination + kShakeOnPerfectCatch).margin(1e-5));
  CHECK(playerIn(rs, 1).flash == Approx(1.0f));

  // Timers count down with dt; shorter effects expire first.
  rs = build(ps, s, s, 1.0f, {}, 0.21f);
  CHECK(countEffects(rs, EffectKind::HitFlash) == 0);           // 0.2 s
  CHECK(countEffects(rs, EffectKind::DashRing) == 1);           // 0.25 s
  CHECK(countEffects(rs, EffectKind::PerfectCatchFlash) == 1);  // 0.4 s
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    CHECK(rs.effects[i].remaining == Approx(rs.effects[i].duration - 0.21f));
    CHECK(rs.effects[i].progress() > 0.0f);
  }
  CHECK(rs.shakeAmplitude < kShakeOnElimination);
  CHECK(playerIn(rs, 1).flash < 1.0f);
  rs = build(ps, s, s, 1.0f, {}, 2.0f);
  CHECK(rs.effectCount == 0);
  CHECK(playerIn(rs, 1).flash == 0.0f);
}

TEST_CASE("a dash hit flashes like a banana hit", "[presentation][builder]") {
  WorldState s = makePlayingWorld(17, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const auto effectsOf = [&](EventKind kind) {
    PresentationState ps;
    build(ps, s, s, 1.0f);
    const std::vector<SimEvent> events = {
        SimEvent{kind, s.tick, kP1, s.players.idAt(0), kP0.value, 0}};
    return build(ps, s, s, 1.0f, events);
  };
  const RenderState banana = effectsOf(EventKind::PlayerHit);
  const RenderState dash = effectsOf(EventKind::PlayerDashHit);
  REQUIRE(dash.effectCount == 1);
  CHECK(countEffects(dash, EffectKind::HitFlash) == 1);
  CHECK(dash.effects[0].slot == 1);
  CHECK(dash.effects[0].duration == banana.effects[0].duration);
  CHECK(dash.effects[0].position.x == banana.effects[0].position.x);
  CHECK(dash.shakeAmplitude == Approx(kShakeOnHit).margin(1e-5));
  CHECK(playerIn(dash, 1).flash == Approx(1.0f));
  CHECK(playerIn(dash, 0).flash == 0.0f);
}

TEST_CASE("the effect buffer never overflows", "[presentation][builder]") {
  const WorldState s = makePlayingWorld(18, 0b1);
  PresentationState ps;
  std::vector<SimEvent> events(
      200, SimEvent{EventKind::PlayerDashed, s.tick, kP0, s.players.idAt(0), 0, 0});
  const RenderState rs = build(ps, s, s, 1.0f, events);
  CHECK(rs.effectCount == kMaxRenderEffects);
}

TEST_CASE("HUD derives phase, countdown, scores and winners", "[presentation][builder]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{19, 0b11});
  stepWith(s, PlayerInput{});  // lobby -> round 1 starting
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  PresentationState ps;
  RenderState rs = build(ps, s, s, 1.0f);
  CHECK(rs.hud.phase == HudPhase::RoundStarting);
  CHECK(rs.hud.round == 1);
  CHECK(rs.hud.roundsToWin == content.modes[0].roundsToWin);
  CHECK(rs.hud.presentMask == 0b11);
  CHECK(rs.hud.ticksInPhase == s.rules.ticksInPhase);
  CHECK(
      rs.hud.countdownSeconds ==
      Approx(static_cast<float>(content.modes[0].roundStartTicks - s.rules.ticksInPhase) / 60.0f));
  CHECK(rs.hud.lastRoundWinner == kNoSlot);
  CHECK(rs.hud.matchWinner == kNoSlot);
  for (int i = 0; i < 30; ++i)
    stepWith(s, PlayerInput{});
  const float earlier = rs.hud.countdownSeconds;
  rs = build(ps, s, s, 1.0f);
  CHECK(rs.hud.countdownSeconds == Approx(earlier - 0.5f));

  // Play out to a decided round by eliminating player 1 by hand.
  s = makePlayingWorld(19, 0b11);
  rs = build(ps, s, s, 1.0f);
  CHECK(rs.hud.phase == HudPhase::Playing);
  CHECK(rs.hud.countdownSeconds == 0.0f);
  playerAt(s, kP1).tags.set(Tag::Eliminated);
  EventLog log = stepWith(s, PlayerInput{});
  REQUIRE(s.rules.phase == MatchPhase::RoundEnded);
  rs = build(ps, s, s, 1.0f, log);
  CHECK(rs.hud.phase == HudPhase::RoundEnded);
  CHECK(rs.hud.lastRoundWinner == 0);
  CHECK(rs.hud.scores[0] == 1);
  CHECK(rs.hud.scores[1] == 0);
  CHECK(rs.hud.roundEndSeconds > 0.0f);
  CHECK(rs.hud.matchWinner == kNoSlot);

  // MatchEnded names the winner.
  s.rules.phase = MatchPhase::MatchEnded;
  s.rules.scores[0] = content.modes[0].roundsToWin;
  rs = build(ps, s, s, 1.0f);
  CHECK(rs.hud.phase == HudPhase::MatchEnded);
  CHECK(rs.hud.matchWinner == 0);
  CHECK(rs.hud.scores[0] == content.modes[0].roundsToWin);
}

TEST_CASE("a scripted 10-player match renders consistently every frame",
          "[presentation][builder]") {
  const ContentDB& content = defaultContent();
  constexpr PlayerMask kTen = (1u << kMaxPlayers) - 1u;
  auto prev = std::make_unique<WorldState>(initialWorld(MatchConfig{0x4B464D10, kTen}));
  auto curr = std::make_unique<WorldState>(*prev);
  auto ps = std::make_unique<PresentationState>();
  auto rs = std::make_unique<RenderState>();
  Rng inputs = Rng::seeded(0x5C121D);
  EventSink sink;
  std::set<EffectKind> seen;
  const float hx = content.arenas[0].halfExtents.x.toFloat();
  const float hy = content.arenas[0].halfExtents.y.toFloat();
  for (int k = 0; k < 2400; ++k) {
    *prev = *curr;
    sink.clear();
    step(*curr, scriptedFrame(inputs, *curr, content), content, sink);
    // Two render frames per tick (a 120 Hz display): halfway, then the tick.
    for (const float alpha : {0.5f, 1.0f}) {
      const std::span<const SimEvent> events =
          alpha < 1.0f ? std::span<const SimEvent>(sink.begin(), sink.count)
                       : std::span<const SimEvent>{};
      buildRenderState(*ps, *prev, *curr, alpha, events, content, 1.0f / 120.0f, *rs);
      REQUIRE(rs->playerCount == curr->players.size());
      REQUIRE(rs->bananaCount == curr->boomerangs.size());
      REQUIRE(rs->pickupCount == curr->pickups.size());
      REQUIRE(rs->shadowCount == rs->playerCount + rs->bananaCount + rs->pickupCount);
      for (uint16_t i = 0; i < rs->playerCount; ++i) {
        const RenderPlayer& p = rs->players[i];
        CHECK(std::fabs(p.ground.x) <= hx);
        CHECK(std::fabs(p.ground.z) <= hy);
        CHECK(p.height >= 0.0f);
        CHECK(p.height <= kDashHopHeight + kHitLiftHeight);
        CHECK(p.colorIndex == p.slot);
        CHECK(p.bodyYaw >= -kPi);
        CHECK(p.bodyYaw <= kPi);
      }
      for (uint16_t i = 0; i < rs->bananaCount; ++i) {
        const RenderBanana& b = rs->bananas[i];
        CHECK(std::fabs(b.ground.x) <= hx);
        CHECK(std::fabs(b.ground.z) <= hy);
        CHECK(b.height >= kBananaHandHeight);
        CHECK(b.height <= kBananaHandHeight + kBananaArcPeak + 1e-4f);
      }
      for (uint16_t i = 0; i < rs->effectCount; ++i) {
        seen.insert(rs->effects[i].kind);
        CHECK(rs->effects[i].remaining > 0.0f);
      }
      CHECK(rs->shakeAmplitude <= kShakeMax);
    }
  }
  CHECK(seen.contains(EffectKind::DashRing));
  CHECK(seen.contains(EffectKind::ThrowPuff));
  CHECK(seen.contains(EffectKind::EliminationBurst));
  CHECK(seen.contains(EffectKind::RoundStartBanner));
  CHECK(seen.contains(EffectKind::RoundEndBanner));
}
