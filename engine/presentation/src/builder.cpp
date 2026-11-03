#include "kfm/presentation/builder.hpp"

#include "kfm/presentation/event_fx.hpp"
#include "kfm/presentation/world_basis.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace kfm::presentation {
namespace {

using sim::AbilityKind;
using sim::AbilityPhase;
using sim::AbilityState;
using sim::Boomerang;
using sim::BoomerangPhase;
using sim::ContentDB;
using sim::Player;
using sim::SimEvent;
using sim::Tag;
using sim::WorldState;

constexpr float kTickSeconds = 1.0f / static_cast<float>(sim::kTickHz);

// The entity at `index` in `pool` if it is alive with generation `gen`.
template <class Pool>
[[nodiscard]] const Pool::value_type* matching(const Pool& pool, uint16_t index, uint16_t gen) {
  if (!pool.isAliveIndex(index) || pool.generation[index] != gen) return nullptr;
  return &pool.items[index];
}

// lerp(prev, curr, alpha), or curr when there is no matching prev or the jump
// is a teleport.
[[nodiscard]] Vec2f interpolate(const Vec2f* prev, Vec2f curr, float alpha) {
  if (prev == nullptr || length(curr - *prev) > kSnapDistance) return curr;
  return lerp(*prev, curr, alpha);
}

// Starts a fresh track when the slot now holds a different entity. Returns
// true for a fresh track.
bool refreshTrack(EntityTrack& t, uint16_t gen) {
  if (t.live && t.generation == gen) return false;
  t = EntityTrack{};
  t.live = true;
  t.generation = gen;
  return true;
}

void decayOffset(EntityTrack& t, float dt) {
  if (t.visualOffset == Vec2f{}) return;
  const float tau = t.tau > 0.0f ? t.tau : kSmoothingTau;
  t.visualOffset = t.visualOffset * std::exp(-dt / tau);
  if (length(t.visualOffset) < kSmoothingEpsilon) t.visualOffset = Vec2f{};
}

[[nodiscard]] const AbilityState& dashSlot(const Player& p) {
  return p.abilities[sim::kAbilitySlotDash];
}

[[nodiscard]] bool dashActive(const Player& p, const ContentDB& content) {
  const AbilityState& st = dashSlot(p);
  return content.abilityAt(st.defIndex).kind == AbilityKind::Dash &&
         st.phase == AbilityPhase::Active;
}

[[nodiscard]] float hitStunFraction(const Player& p, const ContentDB& content) {
  const uint16_t total = content.player.hitStunTicks;
  if (p.hitStunTicks == 0 || total == 0) return 0.0f;
  return clamp01(static_cast<float>(p.hitStunTicks) / static_cast<float>(total));
}

[[nodiscard]] Vec3 playerScale(const Player& p, const ContentDB& content) {
  Vec3 s{1.0f, 1.0f, 1.0f};
  if (dashActive(p, content)) {
    s.x += kDashStretch;
    s.y -= kDashStretch * 0.5f;
    s.z -= kDashStretch * 0.5f;
  }
  const float stun = hitStunFraction(p, content);
  if (stun > 0.0f) {
    s.y *= 1.0f - (kHitSquash * stun);
    s.x *= 1.0f + (kHitSquash * 0.5f * stun);
    s.z *= 1.0f + (kHitSquash * 0.5f * stun);
  }
  return s;
}

[[nodiscard]] uint16_t playerFlags(const Player& p, const ContentDB& content) {
  uint16_t f = 0;
  const auto set = [&](bool on, PlayerFlag flag) {
    if (on) f = static_cast<uint16_t>(f | flagBit(flag));
  };
  set(p.tags.has(Tag::Dashing), PlayerFlag::Dashing);
  set(p.tags.has(Tag::PerfectDash), PlayerFlag::PerfectDash);
  set(p.tags.has(Tag::HitStun), PlayerFlag::HitStun);
  set(p.tags.has(Tag::Eliminated), PlayerFlag::Eliminated);
  set(p.tags.has(Tag::Invulnerable), PlayerFlag::Invulnerable);
  set(p.tags.has(Tag::Shielded), PlayerFlag::Shielded);
  const AbilityState& thr = p.abilities[sim::kAbilitySlotThrow];
  set(content.abilityAt(thr.defIndex).kind == AbilityKind::Throw && thr.phase != AbilityPhase::Idle,
      PlayerFlag::Throwing);
  set(p.ammo > 0, PlayerFlag::HasAmmo);
  return f;
}

[[nodiscard]] float yawOf(Vec2f d) {
  return std::atan2(d.y, d.x);
}

// The direction the body should face: the dash direction while dashing, else
// the stick; `fallback` when idle.
[[nodiscard]] float bodyTargetFor(const Player& p, const ContentDB& content, float fallback) {
  if (dashActive(p, content) && dashSlot(p).dir != Vec2::zero())
    return yawOf(toFloat(dashSlot(p).dir));
  if (p.intent.move != Vec2::zero()) return yawOf(toFloat(p.intent.move));
  return fallback;
}

[[nodiscard]] float ownerDistance(const WorldState& s, const Boomerang& b, float fallback) {
  const Player* owner = s.playerBySlot(b.owner);
  if (owner == nullptr) return fallback;
  return length(toFloat(owner->transform.pos) - toFloat(b.transform.pos));
}

[[nodiscard]] BananaPhase bananaPhase(BoomerangPhase p) {
  switch (p) {
    case BoomerangPhase::Outbound:
      return BananaPhase::Outbound;
    case BoomerangPhase::Returning:
      return BananaPhase::Returning;
    case BoomerangPhase::Grounded:
      break;
  }
  return BananaPhase::Grounded;
}

[[nodiscard]] HudPhase hudPhase(sim::MatchPhase p) {
  switch (p) {
    case sim::MatchPhase::Lobby:
      return HudPhase::Lobby;
    case sim::MatchPhase::RoundStarting:
      return HudPhase::RoundStarting;
    case sim::MatchPhase::Playing:
      return HudPhase::Playing;
    case sim::MatchPhase::RoundEnded:
      return HudPhase::RoundEnded;
    case sim::MatchPhase::MatchEnded:
      break;
  }
  return HudPhase::MatchEnded;
}

[[nodiscard]] int8_t slotOrNone(PlayerId id) {
  return id.isValid() && id.value < sim::kMaxPlayers ? static_cast<int8_t>(id.value) : kNoSlot;
}

[[nodiscard]] float shadowOpacity(float height) {
  return 1.0f - (0.5f * clamp01(height / 2.0f));
}

void pushShadow(RenderState& out, Vec3 ground, float radius, float height, ShadowOwner owner) {
  if (out.shadowCount >= kMaxRenderShadows) return;
  out.shadows[out.shadowCount++] = RenderShadow{ground, radius, shadowOpacity(height), owner};
}

// --- effects ------------------------------------------------------------------

void ageEffects(PresentationState& ps, float dt) {
  uint16_t kept = 0;
  for (uint16_t i = 0; i < ps.effectCount; ++i) {
    RenderEffect e = ps.effects[i];
    e.remaining -= dt;
    if (e.remaining > 0.0f) ps.effects[kept++] = e;
  }
  ps.effectCount = kept;
}

// Counts a trail timer down; true when an afterimage is due (and rearms it).
[[nodiscard]] bool trailDue(float& timer, float dt, float interval) {
  timer -= dt;
  if (timer > 0.0f) return false;
  timer = std::max(timer + interval, 0.0f);
  return true;
}

// Count-in on entering the countdown and on every whole second it crosses;
// "go" when play starts.
void phaseSounds(PresentationState& ps, const RenderHud& hud, RenderState& out) {
  const int whole = hud.phase == HudPhase::RoundStarting
                        ? std::max(1, static_cast<int>(std::ceil(hud.countdownSeconds)))
                        : 0;
  if (ps.hudSeen) {
    if (hud.phase == HudPhase::RoundStarting &&
        (ps.lastHudPhase != HudPhase::RoundStarting || whole < ps.lastCountdownWhole)) {
      pushSound(out, SoundCue{SoundId::CountIn, 1.0f, 0.0f});
    }
    if (hud.phase == HudPhase::Playing && ps.lastHudPhase == HudPhase::RoundStarting) {
      pushSound(out, SoundCue{SoundId::RoundGo, 1.0f, 0.0f});
    }
  }
  ps.hudSeen = true;
  ps.lastHudPhase = hud.phase;
  ps.lastCountdownWhole = whole;
}

// --- entities -------------------------------------------------------------------

void buildPlayers(PresentationState& ps, const WorldState& prev, const WorldState& curr,
                  float alpha, const ContentDB& content, float dt, RenderState& out) {
  const float radius = content.player.radius.toFloat();
  for (uint16_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (!curr.players.isAliveIndex(i)) continue;
    const uint16_t gen = curr.players.generation[i];
    const Player& cp = curr.players.items[i];
    const Player* pp = matching(prev.players, i, gen);
    PlayerTrack& track = ps.players[i];

    const Vec2f currPos = toFloat(cp.transform.pos);
    const Vec2f prevPos = pp != nullptr ? toFloat(pp->transform.pos) : currPos;
    const Vec2f simPos = interpolate(pp != nullptr ? &prevPos : nullptr, currPos, alpha);
    const bool snapped = pp == nullptr || length(currPos - prevPos) > kSnapDistance;

    // A fresh entity or a teleport (round reset, respawn) starts facing its
    // sim facing with no correction pending.
    const bool fresh = refreshTrack(track.motion, gen);
    if (fresh || snapped) {
      track.motion.visualOffset = Vec2f{};
      const float facing = simAngleToYaw(cp.transform.facing);
      track.bodyYaw = facing;
      track.bodyTarget = facing;
    }
    if (fresh) {
      track.flashRemaining = 0.0f;
      track.flashDuration = 0.0f;
      track.trailTimer = 0.0f;
    }
    decayOffset(track.motion, dt);
    track.bodyTarget = bodyTargetFor(cp, content, track.bodyTarget);
    track.bodyYaw = approachAngle(track.bodyYaw, track.bodyTarget, kBodyTurnRate * dt);
    track.flashRemaining = std::max(0.0f, track.flashRemaining - dt);

    const Vec2f drawn = simPos + track.motion.visualOffset;
    track.motion.lastDrawn = drawn;

    const float hCurr = playerHeight(cp, content);
    const float height =
        snapped || pp == nullptr ? hCurr : lerp(playerHeight(*pp, content), hCurr, alpha);

    RenderPlayer& rp = out.players[out.playerCount++];
    rp = RenderPlayer{};
    rp.slot = static_cast<uint8_t>(i);
    rp.colorIndex = static_cast<uint8_t>(i);
    rp.flags = playerFlags(cp, content);
    rp.visible = !cp.tags.has(Tag::Eliminated);
    rp.position = simToWorld(drawn, height);
    rp.ground = simToWorld(simPos);
    rp.height = height;
    rp.bodyYaw = track.bodyYaw;
    rp.aimYaw = simAngleToYaw(cp.intent.aim);
    rp.scale = playerScale(cp, content);
    rp.ammo = cp.ammo;
    rp.ammoMax = cp.stats.ammoMax;
    rp.hp = cp.hp;
    rp.maxHp = cp.stats.maxHp;
    const AbilityState& dash = dashSlot(cp);
    const uint16_t cd = content.abilityAt(dash.defIndex).cooldownTicks;
    rp.dashCooldown =
        cd > 0 ? clamp01(static_cast<float>(dash.cooldownTicks) / static_cast<float>(cd)) : 0.0f;
    pushShadow(out, rp.ground, radius, height, ShadowOwner::Player);

    // Dash trail: afterimages while the Dash is active.
    if (dashActive(cp, content) && !cp.tags.has(Tag::Eliminated)) {
      if (trailDue(track.trailTimer, dt, kDashTrailInterval)) {
        const Vec2f dir = toFloat(dashSlot(cp).dir);
        addEffect(ps, RenderEffect{EffectKind::DashTrail, static_cast<int8_t>(i),
                                   simToWorld(drawn, height + (kEffectBodyHeight * 0.6f)), dir,
                                   kDashTrailSeconds, kDashTrailSeconds});
      }
    } else {
      track.trailTimer = 0.0f;  // the next dash leaves an afterimage at once
    }
  }
}

void buildBananas(PresentationState& ps, const WorldState& prev, const WorldState& curr,
                  float alpha, const ContentDB& content, float dt, RenderState& out) {
  const float radius = content.boomerang.radius.toFloat();
  for (uint16_t i = 0; i < sim::kMaxBoomerangs; ++i) {
    if (!curr.boomerangs.isAliveIndex(i)) {
      ps.bananas[i].motion.live = false;
      continue;
    }
    const uint16_t gen = curr.boomerangs.generation[i];
    const Boomerang& cb = curr.boomerangs.items[i];
    const Boomerang* pb = matching(prev.boomerangs, i, gen);
    BananaTrack& track = ps.bananas[i];
    if (refreshTrack(track.motion, gen)) {
      track.returnStartDistance = -1.0f;
      track.trailTimer = 0.0f;
    }
    decayOffset(track.motion, dt);

    const Vec2f currPos = toFloat(cb.transform.pos);
    const Vec2f prevPos = pb != nullptr ? toFloat(pb->transform.pos) : currPos;
    const Vec2f simPos = interpolate(pb != nullptr ? &prevPos : nullptr, currPos, alpha);
    const bool lerped = pb != nullptr && length(currPos - prevPos) <= kSnapDistance;

    const float dCurr = ownerDistance(curr, cb, track.returnStartDistance);
    if (cb.phase == BoomerangPhase::Returning && track.returnStartDistance < 0.0f)
      track.returnStartDistance = std::max(dCurr, 0.01f);
    const float arcCurr = bananaArcHeight(cb, dCurr, track.returnStartDistance, content);
    float arc = arcCurr;
    float heading = simAngleToYaw(cb.transform.facing);
    float spin = simAngleToYaw(cb.spin);
    if (lerped) {
      const float dPrev = ownerDistance(prev, *pb, track.returnStartDistance);
      arc = lerp(bananaArcHeight(*pb, dPrev, track.returnStartDistance, content), arcCurr, alpha);
      heading = lerpAngle(simAngleToYaw(pb->transform.facing), heading, alpha);
      spin = lerpAngle(simAngleToYaw(pb->spin), spin, alpha);
    }
    const float height =
        cb.phase == BoomerangPhase::Grounded ? kPickupHeight : kBananaHandHeight + arc;

    const Vec2f drawn = simPos + track.motion.visualOffset;
    track.motion.lastDrawn = drawn;

    RenderBanana& rb = out.bananas[out.bananaCount++];
    rb.position = simToWorld(drawn, height);
    rb.ground = simToWorld(simPos);
    rb.height = height;
    rb.headingYaw = heading;
    rb.spin = spin;
    rb.phase = bananaPhase(cb.phase);
    rb.ownerSlot = slotOrNone(cb.owner);
    pushShadow(out, rb.ground, radius, height, ShadowOwner::Banana);

    // A short trail behind a flying banana.
    if (cb.phase != BoomerangPhase::Grounded &&
        trailDue(track.trailTimer, dt, kBananaTrailInterval)) {
      addEffect(ps, RenderEffect{EffectKind::BananaTrail, rb.ownerSlot, rb.position,
                                 Vec2f{std::cos(heading), std::sin(heading)}, kBananaTrailSeconds,
                                 kBananaTrailSeconds});
    }
  }
}

void buildPickups(PresentationState& ps, const WorldState& curr, const ContentDB& content, float dt,
                  RenderState& out) {
  const float radius = content.pickup.radius.toFloat();
  for (uint16_t i = 0; i < sim::kMaxPickups; ++i) {
    if (!curr.pickups.isAliveIndex(i)) {
      ps.pickups[i].live = false;
      continue;
    }
    EntityTrack& track = ps.pickups[i];
    refreshTrack(track, curr.pickups.generation[i]);
    decayOffset(track, dt);
    const sim::Pickup& cp = curr.pickups.items[i];
    const Vec2f simPos = toFloat(cp.pos);
    const Vec2f drawn = simPos + track.visualOffset;
    track.lastDrawn = drawn;
    const float phase = static_cast<float>(i) * 0.7f;
    const float bob = kPickupBobHeight * std::sin((kTwoPi * kPickupBobHz * ps.time) + phase);
    const float height = kPickupHeight + bob;

    RenderPickup& rp = out.pickups[out.pickupCount++];
    rp.position = simToWorld(drawn, height);
    rp.ground = simToWorld(simPos);
    rp.spin = wrapRadians((kPickupSpinRadPerSec * ps.time) + phase);
    rp.radius = radius;
    rp.kind = PickupKindView::Banana;
    pushShadow(out, rp.ground, radius, height, ShadowOwner::Pickup);
  }
}

void buildHud(const WorldState& curr, const sim::MatchContent& content, RenderHud& hud) {
  const sim::MatchRules& r = curr.rules;
  hud.phase = hudPhase(r.phase);
  hud.ticksInPhase = r.ticksInPhase;
  hud.round = r.round;
  hud.roundsToWin = content.mode.roundsToWin;
  hud.presentMask = r.present;
  hud.scores = r.scores;
  hud.lastRoundWinner = slotOrNone(r.lastRoundWinner);
  hud.matchWinner = r.phase == sim::MatchPhase::MatchEnded ? hud.lastRoundWinner : kNoSlot;
  const auto secondsLeft = [&](uint32_t total) {
    const uint32_t left = r.ticksInPhase < total ? total - r.ticksInPhase : 0;
    return static_cast<float>(left) * kTickSeconds;
  };
  hud.countdownSeconds =
      r.phase == sim::MatchPhase::RoundStarting ? secondsLeft(content.mode.roundStartTicks) : 0.0f;
  hud.roundEndSeconds =
      r.phase == sim::MatchPhase::RoundEnded ? secondsLeft(content.mode.roundEndTicks) : 0.0f;

  const sim::ModeDef& mode = content.mode;
  hud.mode = mode.kind;
  hud.scoreToWin = mode.scoreToWin;
  hud.hasClock = mode.kind != sim::ModeKind::Rounds && mode.timeLimitTicks > 0;
  hud.clockSeconds = 0.0f;
  if (hud.hasClock) {
    if (r.phase == sim::MatchPhase::Playing) {
      hud.clockSeconds = secondsLeft(mode.timeLimitTicks);
    } else if (r.phase == sim::MatchPhase::RoundStarting || r.phase == sim::MatchPhase::Lobby) {
      hud.clockSeconds = static_cast<float>(mode.timeLimitTicks) * kTickSeconds;
    }
  }
  hud.aliveCount = 0;
  curr.players.forEachAlive([&](EntityId /*id*/, const Player& p) {
    if (!p.tags.has(sim::Tag::Eliminated)) ++hud.aliveCount;
  });
  hud.deaths = r.deaths;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    hud.respawnSeconds[i] = static_cast<float>(r.respawnTicks[i]) * kTickSeconds;
  }
}

}  // namespace

float dashHopHeight(const Player& p, const ContentDB& content) {
  if (!dashActive(p, content)) return 0.0f;
  const uint16_t active = content.abilityAt(dashSlot(p).defIndex).activeTicks;
  const float u = static_cast<float>(dashSlot(p).phaseTicks + 1) / static_cast<float>(active + 1);
  return kDashHopHeight * std::sin(kPi * clamp01(u));
}

float hitLiftHeight(const Player& p, const ContentDB& content) {
  const uint16_t total = content.player.hitStunTicks;
  if (p.hitStunTicks == 0 || total == 0) return 0.0f;
  const float u = 1.0f - (static_cast<float>(p.hitStunTicks) / static_cast<float>(total + 1));
  return kHitLiftHeight * std::sin(kPi * clamp01(u));
}

float playerHeight(const Player& p, const ContentDB& content) {
  return dashHopHeight(p, content) + hitLiftHeight(p, content);
}

float bananaArcHeight(const Boomerang& b, float distanceToOwner, float returnStartDistance,
                      const ContentDB& content) {
  switch (b.phase) {
    case BoomerangPhase::Outbound: {
      const uint16_t out = content.boomerang.outboundTicks;
      if (out == 0) return kBananaArcPeak;
      const float u = clamp01(static_cast<float>(b.ticksInPhase) / static_cast<float>(out));
      return kBananaArcPeak * std::sin(0.5f * kPi * u);
    }
    case BoomerangPhase::Returning: {
      if (returnStartDistance <= 0.0f) return kBananaArcPeak;
      const float u = clamp01(distanceToOwner / returnStartDistance);
      return kBananaArcPeak * std::sin(0.5f * kPi * u);
    }
    case BoomerangPhase::Grounded:
      break;
  }
  return 0.0f;
}

void buildRenderState(PresentationState& ps, const WorldState& prev, const WorldState& curr,
                      float alpha, std::span<const SimEvent> newEvents,
                      const sim::MatchContent& match, float dtSeconds, RenderState& out) {
  const ContentDB& content = match.db;
  const float dt = std::max(dtSeconds, 0.0f);
  alpha = clamp01(alpha);
  ps.time += dt;

  out.playerCount = 0;
  out.bananaCount = 0;
  out.pickupCount = 0;
  out.shadowCount = 0;
  out.effectCount = 0;
  out.soundCount = 0;

  // Effects: age the running ones first; trails sampled by the entity passes
  // and the new events' effects then start at full duration. Player tracks
  // are still valid for players eliminated this tick.
  ageEffects(ps, dt);
  buildPlayers(ps, prev, curr, alpha, content, dt, out);
  buildBananas(ps, prev, curr, alpha, content, dt, out);
  buildPickups(ps, curr, content, dt, out);

  ps.shake *= std::exp(-dt / kShakeTau);
  for (const SimEvent& e : newEvents)
    applyEventFx(ps, prev, curr, e, match, out);
  // Flashes started by this frame's events show this frame.
  for (uint16_t i = 0; i < out.playerCount; ++i) {
    RenderPlayer& rp = out.players[i];
    const PlayerTrack& t = ps.players[rp.slot];
    rp.flash = t.flashDuration > 0.0f ? clamp01(t.flashRemaining / t.flashDuration) : 0.0f;
  }
  for (uint16_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (!curr.players.isAliveIndex(i)) ps.players[i].motion.live = false;
  }
  if (ps.shake < 1e-4f) ps.shake = 0.0f;

  out.effectCount = ps.effectCount;
  std::copy_n(ps.effects.begin(), ps.effectCount, out.effects.begin());
  out.shakeAmplitude = ps.shake;
  const float w = kTwoPi * kShakeHz * ps.time;
  out.shakeOffset =
      Vec3{ps.shake * std::sin(w), ps.shake * 0.6f * std::sin((1.37f * w) + 1.1f), 0.0f};
  out.arenaHalfExtents = toFloat(match.arena.halfExtents);
  buildArenaPieces(match.arena, out);
  out.playerRadius = content.player.radius.toFloat();
  out.playerHeight = out.playerRadius * kMonkeyHeightOverRadius;
  out.bananaRadius = content.boomerang.radius.toFloat();
  out.time = ps.time;
  buildHud(curr, match, out.hud);
  phaseSounds(ps, out.hud, out);
}

namespace {

struct Box2f {
  Vec2f lo;
  Vec2f hi;
};

[[nodiscard]] float axisOf(Vec2f v, int axis) {
  return axis == 0 ? v.x : v.y;
}

// The stretches of one pit edge that border open floor. The edge lies at
// `at` on `axis` (0 = x, 1 = y), spans [from, to] on the other axis, and
// faces `outward` (+1 or -1). A stretch is covered when another pit holds
// the points just outside it, or when the edge lies on the outer box.
template <class Emit>
void floorStretches(std::span<const Box2f> pits, size_t self, int axis, float at, float outward,
                    float from, float to, float halfExtent, Emit&& emit) {
  if (std::fabs(at) >= halfExtent) return;  // on the outer wall
  std::array<std::pair<float, float>, sim::kMaxArenaHazards> covered{};
  size_t n = 0;
  const int other = 1 - axis;
  for (size_t j = 0; j < pits.size(); ++j) {
    if (j == self) continue;
    const float lo = axisOf(pits[j].lo, axis);
    const float hi = axisOf(pits[j].hi, axis);
    const bool holdsOutside = outward > 0.0f ? (lo <= at && hi > at) : (lo < at && hi >= at);
    if (!holdsOutside) continue;
    const float a = std::max(from, axisOf(pits[j].lo, other));
    const float b = std::min(to, axisOf(pits[j].hi, other));
    if (a < b) covered[n++] = {a, b};
  }
  std::sort(covered.begin(), covered.begin() + static_cast<std::ptrdiff_t>(n));
  float cursor = from;
  for (size_t k = 0; k < n; ++k) {
    if (covered[k].first > cursor) emit(cursor, covered[k].first);
    cursor = std::max(cursor, covered[k].second);
  }
  if (cursor < to) emit(cursor, to);
}

}  // namespace

void buildArenaPieces(const sim::ArenaDef& arena, RenderState& out) {
  uint16_t n = 0;
  const auto add = [&](ArenaPieceKind kind, Vec2f lo, Vec2f hi, float height) {
    if (n >= out.arenaPieces.size() || lo.x >= hi.x || lo.y >= hi.y) return;
    // Sim y maps to world -z, so the box's z extent is its sim y extent.
    out.arenaPieces[n++] =
        RenderArenaPiece{.kind = kind,
                         .center = simToWorld(Vec2f{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y)}),
                         .size = Vec3{hi.x - lo.x, height, hi.y - lo.y}};
  };
  for (uint8_t i = 0; i < arena.wallCount && i < sim::kMaxArenaWalls; ++i) {
    add(ArenaPieceKind::Wall, toFloat(arena.walls[i].min), toFloat(arena.walls[i].max),
        kArenaWallHeight);
  }
  std::array<Box2f, sim::kMaxArenaHazards> pits{};
  size_t pitCount = 0;
  for (uint8_t i = 0; i < arena.hazardCount && i < sim::kMaxArenaHazards; ++i) {
    const sim::Hazard& h = arena.hazards[i];
    const bool pit = h.kind == sim::HazardKind::Pit;
    add(pit ? ArenaPieceKind::Pit : ArenaPieceKind::Slow, toFloat(h.min), toFloat(h.max),
        kHazardSlabHeight);
    if (pit) pits[pitCount++] = Box2f{toFloat(h.min), toFloat(h.max)};
  }
  const std::span<const Box2f> pitSpan(pits.data(), pitCount);
  const Vec2f half = toFloat(arena.halfExtents);
  const float rim = kPitRimWidth;
  uint16_t rims = 0;
  const auto addRim = [&](Vec2f lo, Vec2f hi) {
    if (rims >= kMaxRenderPitRims) return;
    ++rims;
    add(ArenaPieceKind::PitRim, lo, hi, kHazardSlabHeight);
  };
  for (size_t i = 0; i < pitCount; ++i) {
    const Box2f& b = pits[i];
    const float wx = std::min(rim, 0.5f * (b.hi.x - b.lo.x));
    const float wy = std::min(rim, 0.5f * (b.hi.y - b.lo.y));
    // -y and +y edges (axis y fixed), then -x and +x (axis x fixed).
    floorStretches(pitSpan, i, 1, b.lo.y, -1.0f, b.lo.x, b.hi.x, half.y,
                   [&](float a, float c) { addRim({a, b.lo.y}, {c, b.lo.y + wy}); });
    floorStretches(pitSpan, i, 1, b.hi.y, 1.0f, b.lo.x, b.hi.x, half.y,
                   [&](float a, float c) { addRim({a, b.hi.y - wy}, {c, b.hi.y}); });
    floorStretches(pitSpan, i, 0, b.lo.x, -1.0f, b.lo.y, b.hi.y, half.x,
                   [&](float a, float c) { addRim({b.lo.x, a}, {b.lo.x + wx, c}); });
    floorStretches(pitSpan, i, 0, b.hi.x, 1.0f, b.lo.y, b.hi.y, half.x,
                   [&](float a, float c) { addRim({b.hi.x - wx, a}, {b.hi.x, c}); });
  }
  out.arenaPieceCount = n;
}

void onRollback(PresentationState& ps, const WorldState& newPrev, const WorldState& newCurr,
                float alpha, sim::PlayerMask localPlayers) {
  alpha = clamp01(alpha);
  const auto correct = [&](EntityTrack& t, const Vec2f* prevPos, Vec2f currPos, float tau) {
    const Vec2f simPos = interpolate(prevPos, currPos, alpha);
    const Vec2f offset = t.lastDrawn - simPos;
    t.visualOffset = length(offset) > kSmoothingSnapDistance ? Vec2f{} : offset;
    t.tau = tau;
  };
  for (uint16_t i = 0; i < sim::kMaxPlayers; ++i) {
    EntityTrack& t = ps.players[i].motion;
    if (!t.live) continue;
    const Player* cp = matching(newCurr.players, i, t.generation);
    if (cp == nullptr) continue;  // gone or replaced: the next build resets the track
    const Player* pp = matching(newPrev.players, i, t.generation);
    const Vec2f prevPos = pp != nullptr ? toFloat(pp->transform.pos) : Vec2f{};
    const bool local = sim::maskHas(localPlayers, PlayerId{static_cast<uint8_t>(i)});
    correct(t, pp != nullptr ? &prevPos : nullptr, toFloat(cp->transform.pos),
            local ? kLocalSmoothingTau : kSmoothingTau);
  }
  for (uint16_t i = 0; i < sim::kMaxBoomerangs; ++i) {
    EntityTrack& t = ps.bananas[i].motion;
    if (!t.live) continue;
    const Boomerang* cb = matching(newCurr.boomerangs, i, t.generation);
    if (cb == nullptr) continue;
    const Boomerang* pb = matching(newPrev.boomerangs, i, t.generation);
    const Vec2f prevPos = pb != nullptr ? toFloat(pb->transform.pos) : Vec2f{};
    correct(t, pb != nullptr ? &prevPos : nullptr, toFloat(cb->transform.pos), kSmoothingTau);
  }
  for (uint16_t i = 0; i < sim::kMaxPickups; ++i) {
    EntityTrack& t = ps.pickups[i];
    if (!t.live) continue;
    const sim::Pickup* cp = matching(newCurr.pickups, i, t.generation);
    if (cp == nullptr) continue;
    correct(t, nullptr, toFloat(cp->pos), kSmoothingTau);
  }
}

}  // namespace kfm::presentation
