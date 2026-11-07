// RenderState derivation (presentation/builder.hpp) over real sim states
// driven through step(): interpolation and snapping, correction smoothing,
// cosmetic height curves, facing, event-driven effects, the HUD, and a
// scripted 10-player match end to end.
#include "kfm/presentation/builder.hpp"

#include "../sim/sim_test_util.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/presentation/event_fx.hpp"
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

// v0.48: a held throw shows how charged it is (v0.53: the charge pose and
// the strain read it), rising to 1 and back off on the release.
TEST_CASE("a charging throw shows its charge", "[presentation][builder][charge]") {
  WorldState s = makePlayingWorld(16, 0b1);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-12), Fixed::zero()});
  PresentationState ps;
  WorldState prev = s;
  float lastCharge = -1.0f;
  for (int k = 0; k < 60; ++k) {
    prev = s;
    const EventLog log = stepWith(s, PlayerInput{0, 0, 0, kThrowBit, 0});
    const RenderState rs = build(ps, prev, s, 1.0f, log);
    const RenderPlayer& rp = playerIn(rs, 0);
    REQUIRE(rp.has(PlayerFlag::Charging));
    CHECK(rp.throwCharge >= lastCharge);
    lastCharge = rp.throwCharge;
    if (k == 21) {  // 20 ticks past the startup: about half charged
      CHECK(rp.throwCharge > 0.3f);
      CHECK(rp.throwCharge < 0.7f);
    }
  }
  CHECK(lastCharge == 1.0f);  // 58 ticks past the startup: full
  prev = s;
  const EventLog log = stepWith(s, PlayerInput{});
  REQUIRE(countEvents(log, EventKind::BoomerangThrown) == 1);
  const RenderState released = build(ps, prev, s, 1.0f, log);
  CHECK_FALSE(playerIn(released, 0).has(PlayerFlag::Charging));
  CHECK(playerIn(released, 0).throwCharge == 0.0f);

  // A tap: charging for its startup at charge 0.
  WorldState t = makePlayingWorld(17, 0b1);
  prev = t;
  const EventLog tapLog = stepWith(t, PlayerInput{0, 0, 0, kThrowBit, 0});
  const RenderState tapState = build(ps, prev, t, 1.0f, tapLog);
  REQUIRE(playerIn(tapState, 0).has(PlayerFlag::Charging));
  CHECK(playerIn(tapState, 0).throwCharge == 0.0f);
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
  // BoomerangGrounded of an unknown banana has no position: a sound, no dust.
  CHECK(rs.effectCount == 10);
  CHECK(countEffects(rs, EffectKind::DashRing) == 1);
  CHECK(countEffects(rs, EffectKind::DashAnticipation) == 1);
  CHECK(countEffects(rs, EffectKind::PerfectCatchFlash) == 1);
  CHECK(countEffects(rs, EffectKind::PerfectCatchBurst) == 1);
  CHECK(countEffects(rs, EffectKind::HitFlash) == 1);
  CHECK(countEffects(rs, EffectKind::HitSpark) == 1);
  CHECK(countEffects(rs, EffectKind::ThrowPuff) == 1);
  CHECK(countEffects(rs, EffectKind::EliminationBurst) == 1);
  CHECK(countEffects(rs, EffectKind::EliminationPoof) == 1);
  CHECK(countEffects(rs, EffectKind::RoundEndBanner) == 1);
  CHECK(countEffects(rs, EffectKind::GroundDust) == 0);
  CHECK(rs.soundCount == 7);
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
  CHECK(countEffects(rs, EffectKind::DashAnticipation) == 0);   // 0.12 s
  CHECK(countEffects(rs, EffectKind::HitFlash) == 0);           // 0.2 s
  CHECK(countEffects(rs, EffectKind::DashRing) == 1);           // 0.25 s
  CHECK(countEffects(rs, EffectKind::HitSpark) == 1);           // 0.25 s
  CHECK(countEffects(rs, EffectKind::PerfectCatchFlash) == 1);  // 0.4 s
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    CHECK(rs.effects[i].remaining == Approx(rs.effects[i].duration - 0.21f));
    CHECK(rs.effects[i].progress() > 0.0f);
  }
  CHECK(rs.soundCount == 0);  // sounds are cued once, on the event's frame
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
  REQUIRE(dash.effectCount == 2);
  CHECK(countEffects(dash, EffectKind::HitFlash) == 1);
  CHECK(countEffects(dash, EffectKind::HitSpark) == 1);
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
  s.rules.scores[0] = static_cast<int8_t>(content.modes[0].roundsToWin);
  rs = build(ps, s, s, 1.0f);
  CHECK(rs.hud.phase == HudPhase::MatchEnded);
  CHECK(rs.hud.matchWinner == 0);
  CHECK(rs.hud.scores[0] == content.modes[0].roundsToWin);
  CHECK(rs.hud.mode == ModeKind::Rounds);
  CHECK_FALSE(rs.hud.hasClock);
}

TEST_CASE("HUD carries the mode's scoreboard: clock, alive count, respawns",
          "[presentation][builder][modes]") {
  ContentDB content = defaultContent();
  ModeDef& dm = content.modes[1];
  dm.name = ContentName::of("deathmatch");
  dm.kind = ModeKind::Deathmatch;
  dm.scoreToWin = 7;
  dm.timeLimitTicks = 600;
  dm.respawnTicks = 90;
  content.modeCount = 2;
  const MatchContent m{content, 0, 1};

  WorldState s = initialWorld(MatchConfig{.seed = 31, .players = 0b111, .mode = 1});
  EventSink sink;
  InputFrame f;
  f.present = s.rules.present;
  const auto stepOnce = [&] {
    f.tick = s.tick;
    sink.clear();
    step(s, f, m, sink);
  };
  stepOnce();
  REQUIRE(s.rules.phase == MatchPhase::RoundStarting);
  PresentationState ps;
  RenderState rs = buildRenderState(ps, s, s, 1.0f, {}, m, 1.0f / 60.0f);
  CHECK(rs.hud.mode == ModeKind::Deathmatch);
  CHECK(rs.hud.scoreToWin == 7);
  CHECK(rs.hud.hasClock);
  CHECK(rs.hud.clockSeconds == Approx(10.0f));  // the full limit during the countdown
  CHECK(rs.hud.aliveCount == 3);

  while (s.rules.phase != MatchPhase::Playing)
    stepOnce();
  for (int i = 0; i < 60; ++i)
    stepOnce();
  playerAt(s, PlayerId{2}).tags.set(Tag::Eliminated);  // as the damage step would
  s.rules.respawnTicks[2] = 90;
  s.rules.deaths[2] = 1;
  rs = buildRenderState(ps, s, s, 1.0f, {}, m, 1.0f / 60.0f);
  CHECK(rs.hud.clockSeconds == Approx(static_cast<float>(600 - s.rules.ticksInPhase) / 60.0f));
  CHECK(rs.hud.aliveCount == 2);
  CHECK(rs.hud.deaths[2] == 1);
  CHECK(rs.hud.respawnSeconds[2] == Approx(1.5f));
  CHECK(rs.hud.respawnSeconds[0] == 0.0f);
}

TEST_CASE("HUD carries the teams and their scores (v0.32)", "[presentation][builder][teams]") {
  ContentDB content = defaultContent();
  ModeDef& tr = content.modes[1];
  tr.name = ContentName::of("team_rounds");
  tr.kind = ModeKind::TeamRounds;
  tr.teams = 2;
  content.modeCount = 2;
  const MatchConfig config{
      .seed = 5, .players = 0b1111, .mode = 1, .teams = balancedTeams(2, 0b1111)};
  const MatchContent m = matchContent(content, config);
  WorldState s = initialWorld(config);
  s.rules.scores = {1, 0, 2, 1};
  PresentationState ps;
  const RenderState rs = buildRenderState(ps, s, s, 1.0f, {}, m, 1.0f / 60.0f);
  CHECK(rs.hud.teamCount == 2);
  CHECK(rs.hud.teams[2] == 0);
  CHECK(rs.hud.teams[3] == 1);
  CHECK(rs.hud.teamScores[0] == 3);
  CHECK(rs.hud.teamScores[1] == 1);
  CHECK_FALSE(rs.hud.hasClock);
  // A free-for-all: no team count, team i is slot i.
  const RenderState ffa = buildRenderState(ps, s, s, 1.0f, {}, MatchContent(content), 1.0f / 60.0f);
  CHECK(ffa.hud.teamCount == 0);
  CHECK(ffa.hud.teamScores[2] == 2);
}

TEST_CASE("mode events start their effects and sounds", "[presentation][builder][modes]") {
  WorldState s = makePlayingWorld(37, 0b11);
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::fromInt(2)});
  PresentationState ps;
  build(ps, s, s, 1.0f);
  const std::vector<SimEvent> events = {
      SimEvent{EventKind::PlayerRespawned, s.tick, kP1, s.players.idAt(1), 0, 0},
      SimEvent{EventKind::ScoreChanged, s.tick, kP0, EntityId{}, 1, 0},
      SimEvent{EventKind::TimerWarning, s.tick, PlayerId::invalid(), EntityId{}, 10, 0},
  };
  const RenderState rs = build(ps, s, s, 1.0f, events);
  REQUIRE(countEffects(rs, EffectKind::RespawnBurst) == 1);
  CHECK(countEffects(rs, EffectKind::ScorePop) == 1);
  CHECK(countEffects(rs, EffectKind::TimerBanner) == 1);
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    const RenderEffect& e = rs.effects[i];
    if (e.kind != EffectKind::RespawnBurst) continue;
    CHECK(e.slot == 1);
    CHECK(worldToSim(e.position).x == Approx(3.0f));
    CHECK(worldToSim(e.position).y == Approx(2.0f));
  }
  CHECK(bannerScale(rs, EffectKind::TimerBanner) > 1.0f);
  std::set<SoundId> sounds;
  for (uint16_t i = 0; i < rs.soundCount; ++i)
    sounds.insert(rs.sounds[i].id);
  CHECK(sounds == std::set<SoundId>{SoundId::Respawn, SoundId::Score, SoundId::TimerWarning});
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
  std::set<SoundId> heard;
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
      for (uint16_t i = 0; i < rs->soundCount; ++i) {
        const SoundCue& c = rs->sounds[i];
        heard.insert(c.id);
        CHECK(std::fabs(c.pan) <= kPanWidth + 1e-6f);
        CHECK(c.gain > 0.0f);
        CHECK(c.gain <= kSoundStackMax);
      }
      CHECK(rs->shakeAmplitude <= kShakeMax);
    }
  }
  for (const EffectKind k :
       {EffectKind::DashAnticipation, EffectKind::DashTrail, EffectKind::BananaTrail,
        EffectKind::HitSpark, EffectKind::EliminationPoof, EffectKind::GroundDust,
        EffectKind::PickupSparkle}) {
    CHECK(seen.contains(k));
  }
  for (const SoundId id : {SoundId::ThrowWhoosh, SoundId::DashSwish, SoundId::Hit,
                           SoundId::Elimination, SoundId::BananaGround, SoundId::Pickup,
                           SoundId::CountIn, SoundId::RoundGo, SoundId::RoundEnd}) {
    CHECK(heard.contains(id));
  }
  CHECK(seen.contains(EffectKind::DashRing));
  CHECK(seen.contains(EffectKind::ThrowPuff));
  CHECK(seen.contains(EffectKind::EliminationBurst));
  CHECK(seen.contains(EffectKind::RoundStartBanner));
  CHECK(seen.contains(EffectKind::RoundEndBanner));
}

TEST_CASE("dashing players and flying bananas leave trails", "[presentation][builder][vfx]") {
  WorldState s = makePlayingWorld(21, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  PresentationState ps;
  WorldState prev = s;
  RenderState rs = build(ps, prev, s, 1.0f);
  CHECK(countEffects(rs, EffectKind::DashTrail) == 0);

  // Dash: the press frame has the anticipation, the active ticks leave
  // afterimages roughly every kDashTrailInterval.
  PlayerInput dash{};
  dash.buttons = kDashBit;
  prev = s;
  EventLog log = stepWith(s, dash);
  rs = build(ps, prev, s, 1.0f, log);
  CHECK(countEffects(rs, EffectKind::DashAnticipation) == 1);
  CHECK(countEffects(rs, EffectKind::DashTrail) == 1);
  int trails = 0;
  for (int k = 0; k < 10; ++k) {
    prev = s;
    log = stepWith(s, PlayerInput{});
    rs = build(ps, prev, s, 1.0f, log);
    trails = std::max(trails, countEffects(rs, EffectKind::DashTrail));
  }
  CHECK(trails >= 5);
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    if (rs.effects[i].kind == EffectKind::DashTrail) CHECK(rs.effects[i].slot == 0);
  }
  // After the dash every afterimage fades out and no new one starts.
  for (int k = 0; k < 30; ++k) {
    prev = s;
    log = stepWith(s, PlayerInput{});
    rs = build(ps, prev, s, 1.0f, log);
  }
  CHECK(countEffects(rs, EffectKind::DashTrail) == 0);

  // Throw (a fresh round, the dash may have hit player 1): the banana leaves
  // a trail tinted by its owner while it flies.
  s = makePlayingWorld(21, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::fromInt(4)});
  ps = PresentationState{};
  PlayerInput thr{};
  thr.buttons = kThrowBit;
  bool sawTrail = false;
  for (int k = 0; k < 20; ++k) {
    prev = s;
    log = stepWith(s, k == 0 ? thr : PlayerInput{});
    rs = build(ps, prev, s, 1.0f, log);
    for (uint16_t i = 0; i < rs.effectCount; ++i) {
      if (rs.effects[i].kind != EffectKind::BananaTrail) continue;
      sawTrail = true;
      CHECK(rs.effects[i].slot == 0);
      CHECK(rs.effects[i].position.y >= kBananaHandHeight);
    }
  }
  CHECK(sawTrail);
}

TEST_CASE("pickup, landing and bounce effects sit at the entity", "[presentation][builder][vfx]") {
  WorldState s = makePlayingWorld(22, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const EntityId pickup = s.pickups.spawn();
  REQUIRE(pickup.isValid());
  s.pickups.items[pickup.index].pos = Vec2{Fixed::fromInt(2), Fixed::fromInt(1)};
  PresentationState ps;
  const WorldState prev = s;
  WorldState curr = s;
  curr.pickups.despawn(pickup);  // taken this tick: only prev has it
  const std::vector<SimEvent> events = {
      SimEvent{EventKind::PickupTaken, curr.tick, kP1, pickup, 0, 0},
  };
  build(ps, prev, prev, 1.0f);
  const RenderState rs = build(ps, prev, curr, 1.0f, events);
  REQUIRE(countEffects(rs, EffectKind::PickupSparkle) == 1);
  const RenderEffect& e = rs.effects[0];
  CHECK(worldToSim(e.position).x == Approx(2.0f));
  CHECK(worldToSim(e.position).y == Approx(1.0f));
  CHECK(e.slot == 1);
  REQUIRE(rs.soundCount == 1);
  CHECK(rs.sounds[0].id == SoundId::Pickup);
  CHECK(rs.sounds[0].pan ==
        Approx(soundPan(2.0f, defaultContent().arenas[0].halfExtents.x.toFloat())));
  CHECK(rs.sounds[0].pan > 0.0f);
}

TEST_CASE("sound cues: table, pan and same-frame merging", "[presentation][builder][audio]") {
  using enum EventKind;
  const auto id = [](EventKind k, uint32_t a = 0) {
    // SoundId::Count stands for "no sound".
    return soundForEvent(SimEvent{k, Tick{0}, kP0, EntityId{}, a, 0})
        .value_or(SoundCue{SoundId::Count, 0.0f, 0.0f})
        .id;
  };
  CHECK(id(PlayerDashed) == SoundId::DashSwish);
  CHECK(id(BoomerangThrown) == SoundId::ThrowWhoosh);
  CHECK(id(BoomerangCaught, static_cast<uint32_t>(CatchHow::Perfect)) == SoundId::CatchPerfect);
  CHECK(id(BoomerangCaught, static_cast<uint32_t>(CatchHow::Auto)) == SoundId::CatchAuto);
  CHECK(id(PlayerHit) == SoundId::Hit);
  CHECK(id(PlayerDashHit) == SoundId::Hit);
  CHECK(id(PlayerEliminated) == SoundId::Elimination);
  CHECK(id(PickupTaken) == SoundId::Pickup);
  CHECK(id(BoomerangGrounded) == SoundId::BananaGround);
  CHECK(id(BoomerangBounced) == SoundId::WallBounce);
  CHECK(id(RoundEnded) == SoundId::RoundEnd);
  CHECK(id(MatchEnded) == SoundId::MatchEnd);
  CHECK(id(PlayerRespawned) == SoundId::Respawn);
  CHECK(id(ScoreChanged) == SoundId::Score);
  // v0.57: a point lost to a suicide plays no coin; a regrown banana pops.
  CHECK(soundForEvent(SimEvent{ScoreChanged, Tick{0}, kP0, EntityId{}, 0xFFFFFFFFu,
                               static_cast<uint32_t>(sim::ScoreReason::Suicide)}) == std::nullopt);
  CHECK(id(BananaRegrown) == SoundId::Regrow);
  CHECK(id(BananaVanished) == SoundId::Count);
  CHECK(id(TimerWarning) == SoundId::TimerWarning);
  CHECK(id(PlayerSpawned) == SoundId::Count);
  CHECK(id(PickupSpawned) == SoundId::Count);

  CHECK(soundPan(0.0f, 10.0f) == 0.0f);
  CHECK(soundPan(-10.0f, 10.0f) == Approx(-kPanWidth));
  CHECK(soundPan(50.0f, 10.0f) == Approx(kPanWidth));

  // Ten throws in one frame play one louder whoosh, never more than the cap.
  WorldState s = makePlayingWorld(23, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  PresentationState ps;
  build(ps, s, s, 1.0f);
  const std::vector<SimEvent> events(10, SimEvent{BoomerangThrown, s.tick, kP0, EntityId{}, 0, 0});
  const RenderState rs = build(ps, s, s, 1.0f, events);
  REQUIRE(rs.soundCount == 1);
  CHECK(rs.sounds[0].id == SoundId::ThrowWhoosh);
  CHECK(rs.sounds[0].gain == Approx(kSoundStackMax));
  CHECK(rs.sounds[0].pan < 0.0f);
}

TEST_CASE("count-in follows the round-start countdown, then go", "[presentation][builder][audio]") {
  const ContentDB& content = defaultContent();
  WorldState s = initialWorld(MatchConfig{24, 0b11});
  PresentationState ps;
  RenderState rs = build(ps, s, s, 1.0f);  // lobby: nothing
  CHECK(rs.soundCount == 0);
  std::vector<SoundId> heard;
  for (int k = 0; k < content.modes[0].roundStartTicks + 5; ++k) {
    const WorldState prev = s;
    const EventLog log = stepWith(s, PlayerInput{});
    rs = build(ps, prev, s, 1.0f, log);
    for (uint16_t i = 0; i < rs.soundCount; ++i)
      heard.push_back(rs.sounds[i].id);
  }
  // One count-in per whole second of countdown, then go.
  const auto whole = static_cast<std::size_t>((content.modes[0].roundStartTicks + 59) / 60);
  REQUIRE(heard.size() == whole + 1);
  for (std::size_t i = 0; i < whole; ++i)
    CHECK(heard[i] == SoundId::CountIn);
  CHECK(heard.back() == SoundId::RoundGo);
}

TEST_CASE("banners pop in and settle", "[presentation][builder][vfx]") {
  const WorldState s = makePlayingWorld(25, 0b1);
  PresentationState ps;
  build(ps, s, s, 1.0f);
  const std::vector<SimEvent> events = {
      SimEvent{EventKind::RoundStarted, s.tick, PlayerId::invalid(), EntityId{}, 1, 0}};
  RenderState rs = build(ps, s, s, 1.0f, events);
  CHECK(bannerScale(rs, EffectKind::RoundStartBanner) == Approx(1.0f + kBannerPop));
  CHECK(bannerScale(rs, EffectKind::RoundEndBanner) == 1.0f);
  rs = build(ps, s, s, 1.0f, {}, kRoundStartBannerSeconds * 0.5f);
  CHECK(bannerScale(rs, EffectKind::RoundStartBanner) == Approx(1.0f));
}

TEST_CASE("power-up pickups render per def; players carry their timers",
          "[presentation][builder][powerup]") {
  ContentDB content = defaultContent();
  const uint8_t shield = addPowerUp(content, "shield", 120, {tagGrant(Tag::Shielded)});
  content.powerUps[shield].visual = PowerUpVisual{1, PowerUpShape::Coin};
  const uint8_t speed = addPowerUp(
      content, "speed", 600, {statGrant(StatKind::MoveSpeed, Fixed::fromInt(2), Fixed::zero())});
  content.powerUps[speed].visual = PowerUpVisual{6, PowerUpShape::Diamond};

  WorldState s = makePlayingWorld(24, 0b11, content);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  (void)spawnPickup(s, Vec2{Fixed::zero(), Fixed::fromInt(4)});
  (void)spawnPowerUpPickup(s, Vec2{Fixed::fromInt(5), Fixed::fromInt(-4)}, speed);
  givePowerUp(playerAt(s, kP0), shield, 30);
  givePowerUp(playerAt(s, kP0), speed, 600);

  PresentationState ps;
  const RenderState rs = buildRenderState(ps, s, s, 1.0f, {}, content, kDt);
  REQUIRE(rs.pickupCount == 2);
  const RenderPickup& banana = rs.pickups[0];
  const RenderPickup& power = rs.pickups[1];
  CHECK(banana.kind == PickupKindView::Banana);
  CHECK(power.kind == PickupKindView::PowerUp);
  CHECK(power.powerUp == speed);
  CHECK(power.colorIndex == 6);
  CHECK(power.shape == PowerUpShapeView::Diamond);
  CHECK(worldToSim(power.ground).x == Approx(5.0f));

  const RenderPlayer& p0 = playerIn(rs, 0);
  REQUIRE(p0.powerUpCount == 2);
  CHECK(p0.powerUps[0].powerUp == shield);
  CHECK(p0.powerUps[0].colorIndex == 1);
  CHECK(p0.powerUps[0].shape == PowerUpShapeView::Coin);
  CHECK(p0.powerUps[0].secondsLeft == Approx(0.5f));
  CHECK(p0.powerUps[0].fraction == Approx(0.25f));
  CHECK(p0.powerUps[1].fraction == Approx(1.0f));
  CHECK(playerIn(rs, 1).powerUpCount == 0);
}

TEST_CASE("a power-up pickup bursts and chimes; a broken shield shatters",
          "[presentation][builder][powerup][audio]") {
  using enum EventKind;
  const auto id = [](EventKind k, uint32_t a = 0) {
    return soundForEvent(SimEvent{k, Tick{0}, kP0, EntityId{}, a, 0})
        .value_or(SoundCue{SoundId::Count, 0.0f, 0.0f})
        .id;
  };
  CHECK(id(PickupTaken, static_cast<uint32_t>(PickupKind::Banana)) == SoundId::Pickup);
  CHECK(id(PickupTaken, static_cast<uint32_t>(PickupKind::PowerUp)) == SoundId::PowerUp);
  CHECK(id(ShieldBroken) == SoundId::ShieldBreak);

  WorldState s = makePlayingWorld(25, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const EntityId pickup = spawnPowerUpPickup(s, Vec2{Fixed::fromInt(2), Fixed::one()}, 0);
  PresentationState ps;
  const WorldState prev = s;
  WorldState curr = s;
  curr.pickups.despawn(pickup);
  const std::vector<SimEvent> events = {
      SimEvent{PickupTaken, curr.tick, kP1, pickup, static_cast<uint32_t>(PickupKind::PowerUp), 0},
      SimEvent{ShieldBroken, curr.tick, kP0, EntityId{}, 1, 0},
  };
  build(ps, prev, prev, 1.0f);
  const RenderState rs = build(ps, prev, curr, 1.0f, events);
  CHECK(countEffects(rs, EffectKind::PickupSparkle) == 1);
  REQUIRE(countEffects(rs, EffectKind::PowerUpBurst) == 1);
  REQUIRE(countEffects(rs, EffectKind::ShieldShatter) == 1);
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    const RenderEffect& e = rs.effects[i];
    if (e.kind == EffectKind::PowerUpBurst) {
      CHECK(e.slot == 1);
      CHECK(worldToSim(e.position).x == Approx(2.0f));
    }
    if (e.kind == EffectKind::ShieldShatter) CHECK(e.slot == 0);
  }
  int powerUpSounds = 0;
  int shieldSounds = 0;
  for (uint16_t i = 0; i < rs.soundCount; ++i) {
    if (rs.sounds[i].id == SoundId::PowerUp) ++powerUpSounds;
    if (rs.sounds[i].id == SoundId::ShieldBreak) ++shieldSounds;
  }
  CHECK(powerUpSounds == 1);
  CHECK(shieldSounds == 1);
}

TEST_CASE("arena walls and hazards become world-space pieces", "[presentation][builder][arena]") {
  // The dojo has no statics.
  {
    const WorldState s = makePlayingWorld(26, 0b1);
    PresentationState ps;
    CHECK(build(ps, s, s, 1.0f).arenaPieceCount == 0);
  }
  ArenaDef a = defaultContent().arenas[0];
  const auto at = [](int32_t x, int32_t y) { return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)}; };
  a.walls[0] = Wall{at(2, 1), at(4, 5)};
  a.wallCount = 1;
  // An L of two pits touching along y = 2, the second running into the
  // outer wall (x = 16), and a mud patch.
  a.hazards[0] = Hazard{at(-6, -2), at(-2, 2), HazardKind::Pit, Fixed::one()};
  a.hazards[1] = Hazard{at(-6, 2), at(16, 4), HazardKind::Pit, Fixed::one()};
  a.hazards[2] = Hazard{at(8, -8), at(10, -6), HazardKind::Slow, Fixed::half()};
  a.hazardCount = 3;
  auto rs = std::make_unique<RenderState>();
  buildArenaPieces(a, *rs);

  // Walls first: sim (x, y) box -> world centre (x, 0, -y), size (dx, h, dy).
  REQUIRE(rs->arenaPieceCount >= 4);
  const RenderArenaPiece& wall = rs->arenaPieces[0];
  CHECK(wall.kind == ArenaPieceKind::Wall);
  CHECK(wall.center.x == Approx(3.0f));
  CHECK(wall.center.y == 0.0f);
  CHECK(wall.center.z == Approx(-3.0f));
  CHECK(wall.size.x == Approx(2.0f));
  CHECK(wall.size.y == Approx(kArenaWallHeight));
  CHECK(wall.size.z == Approx(4.0f));
  CHECK(rs->arenaPieces[1].kind == ArenaPieceKind::Pit);
  CHECK(rs->arenaPieces[2].kind == ArenaPieceKind::Pit);
  CHECK(rs->arenaPieces[3].kind == ArenaPieceKind::Slow);
  CHECK(rs->arenaPieces[3].size.y == Approx(kHazardSlabHeight));

  // Rims run only where a pit meets open floor: not along the stretch the
  // two pits share (x in [-6, -2] at y = 2), not along the outer wall.
  constexpr float kEps = 1e-4f;
  float rimLength = 0.0f;
  for (uint16_t i = 4; i < rs->arenaPieceCount; ++i) {
    const RenderArenaPiece& p = rs->arenaPieces[i];
    REQUIRE(p.kind == ArenaPieceKind::PitRim);
    const float x0 = p.center.x - (0.5f * p.size.x);
    const float x1 = p.center.x + (0.5f * p.size.x);
    const float y0 = -p.center.z - (0.5f * p.size.z);
    const float y1 = -p.center.z + (0.5f * p.size.z);
    INFO("rim x [" << x0 << ", " << x1 << "] y [" << y0 << ", " << y1 << "]");
    const bool inFirst =
        x0 >= -6.0f - kEps && x1 <= -2.0f + kEps && y0 >= -2.0f - kEps && y1 <= 2.0f + kEps;
    const bool inSecond =
        x0 >= -6.0f - kEps && x1 <= 16.0f + kEps && y0 >= 2.0f - kEps && y1 <= 4.0f + kEps;
    CHECK((inFirst || inSecond));
    const bool acrossJoint = y0 < 2.0f + kPitRimWidth - kEps && y1 > 2.0f - kPitRimWidth + kEps &&
                             x1 > -6.0f + kPitRimWidth + kEps && x0 < -2.0f - kPitRimWidth - kEps;
    CHECK_FALSE(acrossJoint);
    rimLength += std::max(p.size.x, p.size.z);
  }
  // First pit: its -y, -x and +x edges (4 each); second pit: -y edge from
  // x = -2 to 16 (18), +y edge (22), -x edge (2); its +x edge is the wall.
  CHECK(rimLength == Approx(4.0f + 4.0f + 4.0f + 18.0f + 22.0f + 2.0f));
}

TEST_CASE("a call shouts over the caller and pings only a local human (v0.35)",
          "[presentation][builder][audio]") {
  using enum EventKind;
  CHECK_FALSE(soundForEvent(SimEvent{MonkeyCalled, Tick{0}, kP0, EntityId{}, 9, 0}).has_value());
  WorldState s = makePlayingWorld(28, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const auto pings = [](const RenderState& rs) {
    int n = 0;
    for (uint16_t i = 0; i < rs.soundCount; ++i) {
      if (rs.sounds[i].id == SoundId::CallPing) ++n;
    }
    return n;
  };
  PresentationState ps;
  ps.pingSlots = 0b01;  // P0 is the local human
  const std::vector<SimEvent> events = {
      SimEvent{MonkeyCalled, s.tick, kP0, EntityId{}, 255, 0},
      SimEvent{MonkeyCalled, s.tick, kP1, EntityId{}, 0, 0},
  };
  build(ps, s, s, 1.0f);
  const RenderState rs = build(ps, s, s, 1.0f, events);
  REQUIRE(countEffects(rs, EffectKind::CallShout) == 2);
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    const RenderEffect& e = rs.effects[i];
    if (e.kind != EffectKind::CallShout) continue;
    CHECK(e.position.y == Approx(kCallShoutHeight));  // over the head
    if (e.slot == 0) CHECK(e.size == Approx(1.0f));
    if (e.slot == 1) CHECK(e.size == Approx(0.0f));
  }
  CHECK(pings(rs) == 1);  // the bot's call is silent
}

TEST_CASE("ability events (v0.21): blink, slam and parry effects and sounds",
          "[presentation][builder][audio]") {
  using enum EventKind;
  const auto id = [](EventKind k) {
    return soundForEvent(SimEvent{k, Tick{0}, kP0, EntityId{}, 0, 0})
        .value_or(SoundCue{SoundId::Count, 0.0f, 0.0f})
        .id;
  };
  CHECK(id(PlayerBlinked) == SoundId::Blink);
  CHECK(id(SlamLanded) == SoundId::Slam);
  CHECK(id(BoomerangParried) == SoundId::Parry);
  CHECK(id(PlayerSlammed) == SoundId::Count);       // the slam sounds once
  CHECK(id(PlayerParryStarted) == SoundId::Count);  // the stance is silent

  WorldState s = makePlayingWorld(27, 0b11);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-3), Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(3), Fixed::zero()});
  const EntityId banana = spawnBanana(s, kP0, Vec2{Fixed::fromInt(2), Fixed::zero()},
                                      Vec2{Fixed::one(), Fixed::zero()});
  PresentationState ps;
  const WorldState prev = s;
  const WorldState curr = s;
  // P0 blinked from (-8, 0) to (-3, 0).
  const Fixed fromX = Fixed::fromInt(-8);
  const std::vector<SimEvent> events = {
      SimEvent{PlayerBlinked, curr.tick, kP0, EntityId{}, static_cast<uint32_t>(fromX.raw), 0},
      SimEvent{SlamLanded, curr.tick, kP1, EntityId{}, static_cast<uint32_t>(Fixed::fromInt(3).raw),
               0},
      SimEvent{PlayerSlammed, curr.tick, kP0, EntityId{}, 1, 0},
      SimEvent{PlayerParryStarted, curr.tick, kP0, EntityId{}, 0, 0},
      SimEvent{BoomerangParried, curr.tick, kP0, banana, 1, 0},
  };
  build(ps, prev, prev, 1.0f);
  const RenderState rs = build(ps, prev, curr, 1.0f, events);
  REQUIRE(countEffects(rs, EffectKind::BlinkFlash) == 2);
  REQUIRE(countEffects(rs, EffectKind::SlamShockwave) == 1);
  CHECK(countEffects(rs, EffectKind::ParryGuard) == 1);
  CHECK(countEffects(rs, EffectKind::ParrySpark) == 1);
  float blinkXs = 0.0f;
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    const RenderEffect& e = rs.effects[i];
    if (e.kind == EffectKind::BlinkFlash) {
      CHECK(e.slot == 0);
      blinkXs += worldToSim(e.position).x;
    }
    if (e.kind == EffectKind::SlamShockwave) {
      CHECK(e.slot == 1);
      CHECK(e.size == Approx(3.0f));
      CHECK(worldToSim(e.position).x == Approx(3.0f));
    }
    if (e.kind == EffectKind::ParrySpark) CHECK(worldToSim(e.position).x == Approx(2.0f));
  }
  CHECK(blinkXs == Approx(-11.0f));  // both ends: -8 and -3
  int blink = 0;
  int slam = 0;
  int parry = 0;
  for (uint16_t i = 0; i < rs.soundCount; ++i) {
    if (rs.sounds[i].id == SoundId::Blink) ++blink;
    if (rs.sounds[i].id == SoundId::Slam) ++slam;
    if (rs.sounds[i].id == SoundId::Parry) ++parry;
  }
  CHECK(blink == 1);
  CHECK(slam == 1);
  CHECK(parry == 1);
}

TEST_CASE("zones and capture flags render (v0.33)", "[presentation][builder][capture]") {
  ContentDB content = defaultContent();
  const auto at = [](int32_t x, int32_t y) { return Vec2{Fixed::fromInt(x), Fixed::fromInt(y)}; };
  ArenaDef& a = content.arenas[0];
  a.zones[0] = Zone{at(-14, -2), at(-10, 2)};
  a.zones[1] = Zone{at(10, -1), at(14, 3)};
  a.zoneCount = 2;
  content.modes[1] = content.modes[0];
  content.modes[1].name = ContentName::of("capture");
  content.modes[1].kind = ModeKind::Capture;
  content.modes[1].teams = 2;
  content.modeCount = 2;
  WorldState s = makePlayingWorld(24, 0b11, content);
  const EntityId flag = s.pickups.spawn();
  REQUIRE(flag.isValid());
  s.pickups.items[flag.index] = Pickup{PickupKind::Flag, at(12, 1), 0, 1};
  playerAt(s, kP0).tags.set(Tag::Carrying);

  // In a free-for-all mode the zones are nobody's.
  {
    auto rs = std::make_unique<RenderState>();
    buildZones(MatchContent(content), *rs);
    REQUIRE(rs->zoneCount == 2);
    CHECK(rs->zones[0].team == kNoSlot);
    CHECK(rs->zones[1].team == kNoSlot);
  }
  const MatchContent capture(content, 0, 1);
  PresentationState ps;
  const RenderState rs = buildRenderState(ps, s, s, 1.0f, {}, capture, kDt);
  REQUIRE(rs.zoneCount == 2);
  CHECK(rs.zones[0].team == 0);
  CHECK(rs.zones[1].team == 1);
  CHECK(worldToSim(rs.zones[1].center).x == Approx(12.0f));
  CHECK(worldToSim(rs.zones[1].center).y == Approx(1.0f));
  CHECK(rs.zones[1].size.x == Approx(4.0f));
  CHECK(rs.zones[1].size.z == Approx(4.0f));
  REQUIRE(rs.pickupCount == 1);
  CHECK(rs.pickups[0].kind == PickupKindView::Flag);
  CHECK(rs.pickups[0].colorIndex == 1);  // team 1's colour
  CHECK(rs.pickups[0].shape == kFlagShape);
  const RenderPlayer* p0 = nullptr;
  for (uint16_t i = 0; i < rs.playerCount; ++i) {
    if (rs.players[i].slot == 0) p0 = &rs.players[i];
  }
  REQUIRE(p0 != nullptr);
  CHECK(p0->has(PlayerFlag::Carrying));
  // Taking a flag sounds like a power-up.
  const SimEvent taken{
      EventKind::PickupTaken, s.tick, kP1, flag, static_cast<uint32_t>(PickupKind::Flag), 1};
  CHECK(soundForEvent(taken).value_or(SoundCue{SoundId::Count, 0.0f, 0.0f}).id == SoundId::PowerUp);
}

TEST_CASE("a pit fall sinks the monkey under the floor, then bursts from the pit (v0.57)",
          "[presentation][builder][pit]") {
  // The dojo with a pit strip at x in [-9, -7]; P0 stands with its whole
  // body over it, touching the east edge, and goes in on the first tick.
  ContentDB c = defaultContent();
  ArenaDef& a = c.arenas[0];
  a.hazards[0] = Hazard{Vec2{Fixed::fromInt(-9), -a.halfExtents.y},
                        Vec2{Fixed::fromInt(-7), a.halfExtents.y}, HazardKind::Pit, Fixed::one()};
  a.hazardCount = 1;
  const std::optional<SoundCue> cue =
      soundForEvent(SimEvent{EventKind::PlayerFell, Tick{0}, kP0, EntityId{}, 0, 0});
  CHECK(cue.has_value());
  CHECK(cue.value_or(SoundCue{}).id == SoundId::Fall);
  // A faller's banana dropping into its pit plops.
  CHECK(soundForEvent(SimEvent{EventKind::BoomerangFell, Tick{0}, kP0, EntityId{}, 0, 0})
            .value_or(SoundCue{})
            .id == SoundId::BananaPlop);
  WorldState s = makePlayingWorld(57, 0b11, c);
  placePlayer(s, kP0, Vec2{Fixed::fromInt(-7) - c.player.radius, Fixed::zero()});
  placePlayer(s, kP1, Vec2{Fixed::fromInt(8), Fixed::zero()});
  REQUIRE(playerAt(s, kP0).ammo > 0);
  // And one of its bananas in flight over the pit: it comes down as the fall
  // begins and drops into the hole.
  spawnBanana(s, kP0, Vec2{-Fixed::fromInt(8), Fixed::fromInt(3)}, Vec2{}, BoomerangPhase::Outbound,
              c);
  const float radius = c.player.radius.toFloat();
  PresentationState ps;
  float lastFall = -1.0f;
  float deepest = 0.0f;
  bool sawStart = false;
  bool sawBurst = false;
  bool bananaSank = false;
  bool plop = false;
  for (int k = 0; k < c.player.fallTicks + 2 && !sawBurst; ++k) {
    const WorldState prev = s;
    EventLog log;
    runTicks(s, frameFor(s), 1, c, log);
    const RenderState rs = buildRenderState(ps, prev, s, 1.0f, log, c, kDt);
    for (uint16_t i = 0; i < rs.bananaCount; ++i) {
      const RenderBanana& b = rs.bananas[i];
      if (b.phase == BananaPhase::Sinking && b.height < 0.0f) bananaSank = true;
    }
    for (uint16_t i = 0; i < rs.soundCount; ++i)
      plop = plop || rs.sounds[i].id == SoundId::BananaPlop;
    if (findEvent(log, EventKind::PlayerFell) != nullptr) {
      sawStart = true;
      // The monkey's ring and the small splash of its banana coming down over the pit.
      CHECK(countEffects(rs, EffectKind::PitFall) == 2);
      bool fallSound = false;
      for (uint16_t i = 0; i < rs.soundCount; ++i)
        fallSound = fallSound || rs.sounds[i].id == SoundId::Fall;
      CHECK(fallSound);
    }
    if (const RenderPlayer* p = findPlayer(rs, 0)) {
      REQUIRE(p->has(PlayerFlag::Falling));
      CHECK(p->fall >= lastFall);
      lastFall = p->fall;
      deepest = std::min(deepest, p->position.y);
      // It sinks where it stands, its whole body over the pit, the banana
      // still in its hand.
      CHECK(groundOf(*p).x <= -7.0f - radius + 1e-3f);
      CHECK(p->has(PlayerFlag::HasAmmo));
      if (p->fall > 0.5f) {
        CHECK(p->scale.y < 1.0f);
        CHECK(p->tilt < 0.0f);
      }
    } else if (findEvent(log, EventKind::PlayerEliminated) != nullptr) {
      // The burst comes out of the pit at floor level, not at body height.
      for (uint16_t i = 0; i < rs.effectCount; ++i) {
        if (rs.effects[i].kind != EffectKind::EliminationBurst) continue;
        CHECK(rs.effects[i].position.y == Approx(kEffectGroundHeight));
        sawBurst = true;
      }
    }
  }
  CHECK(sawStart);
  CHECK(sawBurst);
  CHECK(bananaSank);
  CHECK(plop);
  CHECK(lastFall > 0.9f);
  // Deep enough under the floor that the body, scaled down, is gone.
  CHECK(deepest < -radius * kMonkeyHeightOverRadius * kFallEndScale);
}
