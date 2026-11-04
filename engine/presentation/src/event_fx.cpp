#include "kfm/presentation/event_fx.hpp"

#include "kfm/presentation/tuning.hpp"
#include "kfm/presentation/world_basis.hpp"

#include <algorithm>

namespace kfm::presentation {
namespace {

using sim::Boomerang;
using sim::EventKind;
using sim::Player;
using sim::SimEvent;
using sim::WorldState;

[[nodiscard]] int8_t slotOf(PlayerId id) {
  return id.isValid() && id.value < sim::kMaxPlayers ? static_cast<int8_t>(id.value) : kNoSlot;
}

[[nodiscard]] RenderEffect makeEffect(EffectKind kind, int8_t slot, Vec3 pos, float seconds,
                                      Vec2f dir = {}) {
  return RenderEffect{kind, slot, pos, dir, seconds, seconds, 0.0f};
}

void addShake(PresentationState& ps, float amount) {
  ps.shake = std::min(ps.shake + amount, kShakeMax);
}

void flashPlayer(PresentationState& ps, PlayerId id, float seconds) {
  if (!id.isValid() || id.value >= sim::kMaxPlayers) return;
  PlayerTrack& t = ps.players[id.value];
  if (seconds >= t.flashRemaining) {
    t.flashRemaining = seconds;
    t.flashDuration = seconds;
  }
}

// Where a player is for an event: drawn this frame if alive, else its
// position in prev (eliminated this tick), else the last drawn position.
[[nodiscard]] Vec2f playerPlane(const PresentationState& ps, const WorldState& prev,
                                const WorldState& curr, PlayerId id) {
  if (!id.isValid() || id.value >= sim::kMaxPlayers) return Vec2f{};
  if (curr.playerBySlot(id) != nullptr) return ps.players[id.value].motion.lastDrawn;
  if (const Player* p = prev.playerBySlot(id)) return toFloat(p->transform.pos);
  return ps.players[id.value].motion.lastDrawn;
}

// A banana's position for an event: in curr, else prev (despawned this
// tick), else its track's last drawn position if the track is that banana.
[[nodiscard]] std::optional<Vec2f> bananaPlane(const PresentationState& ps, const WorldState& prev,
                                               const WorldState& curr, EntityId id) {
  if (const Boomerang* b = curr.boomerangs.get(id)) return toFloat(b->transform.pos);
  if (const Boomerang* b = prev.boomerangs.get(id)) return toFloat(b->transform.pos);
  if (id.isValid() && id.index < sim::kMaxBoomerangs &&
      ps.bananas[id.index].motion.generation == id.generation &&
      ps.bananas[id.index].motion.lastDrawn != Vec2f{}) {
    return ps.bananas[id.index].motion.lastDrawn;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Vec2f> pickupPlane(const PresentationState& ps, const WorldState& prev,
                                               const WorldState& curr, EntityId id) {
  if (const sim::Pickup* p = curr.pickups.get(id)) return toFloat(p->pos);
  if (const sim::Pickup* p = prev.pickups.get(id)) return toFloat(p->pos);
  if (id.isValid() && id.index < sim::kMaxPickups &&
      ps.pickups[id.index].generation == id.generation &&
      ps.pickups[id.index].lastDrawn != Vec2f{}) {
    return ps.pickups[id.index].lastDrawn;
  }
  return std::nullopt;
}

[[nodiscard]] Vec2f directionOr(Vec2f d, Vec2f fallback) {
  const float len = length(d);
  return len > 1e-4f ? d * (1.0f / len) : fallback;
}

}  // namespace

std::optional<SoundCue> soundForEvent(const SimEvent& e) {
  const auto cue = [](SoundId id) { return SoundCue{id, 1.0f, 0.0f}; };
  switch (e.kind) {
    case EventKind::PlayerDashed:
      return cue(SoundId::DashSwish);
    case EventKind::BoomerangThrown:
      return cue(SoundId::ThrowWhoosh);
    case EventKind::BoomerangCaught:
      return cue(e.a == static_cast<uint32_t>(sim::CatchHow::Perfect) ? SoundId::CatchPerfect
                                                                      : SoundId::CatchAuto);
    case EventKind::PlayerHit:
    case EventKind::PlayerDashHit:
      return cue(SoundId::Hit);
    case EventKind::PlayerEliminated:
      return cue(SoundId::Elimination);
    case EventKind::PickupTaken:
      // A power-up or a capture flag (v0.33) sounds the same.
      return cue(e.a != static_cast<uint32_t>(sim::PickupKind::Banana) ? SoundId::PowerUp
                                                                       : SoundId::Pickup);
    case EventKind::ShieldBroken:
      return cue(SoundId::ShieldBreak);
    case EventKind::PlayerBlinked:
      return cue(SoundId::Blink);
    case EventKind::SlamLanded:
      return cue(SoundId::Slam);
    case EventKind::BoomerangParried:
      return cue(SoundId::Parry);
    case EventKind::BoomerangGrounded:
      return cue(SoundId::BananaGround);
    case EventKind::BoomerangBounced:
      return cue(SoundId::WallBounce);
    case EventKind::RoundEnded:
      return cue(SoundId::RoundEnd);
    case EventKind::MatchEnded:
      return cue(SoundId::MatchEnd);
    case EventKind::PlayerRespawned:
      return cue(SoundId::Respawn);
    case EventKind::ScoreChanged:
      return cue(SoundId::Score);
    case EventKind::TimerWarning:
      return cue(SoundId::TimerWarning);
    default:
      break;
  }
  return std::nullopt;
}

float soundPan(float simX, float arenaHalfWidth) {
  if (arenaHalfWidth <= 0.0f) return 0.0f;
  return kPanWidth * std::clamp(simX / arenaHalfWidth, -1.0f, 1.0f);
}

void pushSound(RenderState& out, const SoundCue& cue) {
  for (uint16_t i = 0; i < out.soundCount; ++i) {
    SoundCue& s = out.sounds[i];
    if (s.id != cue.id) continue;
    s.gain = std::min(s.gain + (kSoundStackGain * cue.gain), kSoundStackMax);
    return;
  }
  if (out.soundCount < kMaxRenderSounds) out.sounds[out.soundCount++] = cue;
}

void addEffect(PresentationState& ps, const RenderEffect& e) {
  if (ps.effectCount < kMaxRenderEffects) {
    ps.effects[ps.effectCount++] = e;
    return;
  }
  // Full: replace the effect closest to expiring.
  uint16_t victim = 0;
  for (uint16_t i = 1; i < ps.effectCount; ++i) {
    if (ps.effects[i].remaining < ps.effects[victim].remaining) victim = i;
  }
  ps.effects[victim] = e;
}

float bannerScale(const RenderState& state, EffectKind banner) {
  float scale = 1.0f;
  for (uint16_t i = 0; i < state.effectCount; ++i) {
    const RenderEffect& e = state.effects[i];
    if (e.kind != banner) continue;
    const float settle = 1.0f - clamp01(e.progress() * 4.0f);
    scale = std::max(scale, 1.0f + (kBannerPop * settle * settle));
  }
  return scale;
}

void applyEventFx(PresentationState& ps, const WorldState& prev, const WorldState& curr,
                  const SimEvent& e, const sim::MatchContent& content, RenderState& out) {
  const float halfWidth = content.arena.halfExtents.x.toFloat();
  // The plane position the sound pans from; unset for unpositioned events.
  std::optional<Vec2f> soundAt;
  const auto bodyAt = [&](Vec2f plane) { return simToWorld(plane, kEffectBodyHeight); };
  const auto groundAt = [&](Vec2f plane) { return simToWorld(plane, kEffectGroundHeight); };
  const int8_t slot = slotOf(e.player);
  const Vec2f player = playerPlane(ps, prev, curr, e.player);

  switch (e.kind) {
    case EventKind::PlayerDashed: {
      Vec2f dir{};
      if (const Player* p = curr.playerBySlot(e.player))
        dir = toFloat(p->abilities[sim::kAbilitySlotDash].dir);
      addEffect(ps,
                makeEffect(EffectKind::DashRing, slot, groundAt(player), kDashRingSeconds, dir));
      addEffect(ps, makeEffect(EffectKind::DashAnticipation, slot, groundAt(player),
                               kDashAnticipationSeconds, dir));
      soundAt = player;
      break;
    }
    case EventKind::BoomerangThrown: {
      Vec3 pos = bodyAt(player);
      Vec2f dir{};
      if (const Boomerang* b = curr.boomerangs.get(e.entity)) {
        pos = simToWorld(toFloat(b->transform.pos), kBananaHandHeight);
        dir = toFloat(Vec2::fromAngle(b->transform.facing));
      }
      addEffect(ps, makeEffect(EffectKind::ThrowPuff, slot, pos, kThrowPuffSeconds, dir));
      soundAt = player;
      break;
    }
    case EventKind::BoomerangCaught: {
      const bool perfect = e.a == static_cast<uint32_t>(sim::CatchHow::Perfect);
      const float seconds = perfect ? kPerfectCatchFlashSeconds : kAutoCatchFlashSeconds;
      addEffect(ps, makeEffect(perfect ? EffectKind::PerfectCatchFlash : EffectKind::AutoCatchFlash,
                               slot, bodyAt(player), seconds));
      if (perfect) {
        addEffect(ps, makeEffect(EffectKind::PerfectCatchBurst, slot, bodyAt(player),
                                 kPerfectCatchBurstSeconds));
        addShake(ps, kShakeOnPerfectCatch);
      }
      flashPlayer(ps, e.player, seconds);
      soundAt = player;
      break;
    }
    case EventKind::PlayerHit:
    case EventKind::PlayerDashHit: {  // a dash hit reads like a banana hit (§5.9)
      // Sparks fly away from the banana (PlayerHit) or the attacker.
      std::optional<Vec2f> from;
      if (e.kind == EventKind::PlayerHit) from = bananaPlane(ps, prev, curr, e.entity);
      if (!from && e.a < sim::kMaxPlayers) {
        from = playerPlane(ps, prev, curr, PlayerId{static_cast<uint8_t>(e.a)});
      }
      const Vec2f away = from ? directionOr(player - *from, Vec2f{}) : Vec2f{};
      addEffect(ps, makeEffect(EffectKind::HitFlash, slot, bodyAt(player), kHitFlashSeconds));
      addEffect(ps, makeEffect(EffectKind::HitSpark, slot, bodyAt(player), kHitSparkSeconds, away));
      flashPlayer(ps, e.player, kHitFlashSeconds);
      addShake(ps, kShakeOnHit);
      soundAt = player;
      break;
    }
    case EventKind::PlayerEliminated:
      addEffect(ps, makeEffect(EffectKind::EliminationBurst, slot, bodyAt(player),
                               kEliminationBurstSeconds));
      addEffect(ps, makeEffect(EffectKind::EliminationPoof, slot, bodyAt(player),
                               kEliminationPoofSeconds));
      addShake(ps, kShakeOnElimination);
      soundAt = player;
      break;
    case EventKind::PickupTaken: {
      const Vec2f at = pickupPlane(ps, prev, curr, e.entity).value_or(player);
      addEffect(ps, makeEffect(EffectKind::PickupSparkle, slot, simToWorld(at, kPickupHeight),
                               kPickupSparkleSeconds));
      if (e.a != static_cast<uint32_t>(sim::PickupKind::Banana)) {
        addEffect(ps, makeEffect(EffectKind::PowerUpBurst, slot, simToWorld(at, kPickupHeight),
                                 kPowerUpBurstSeconds));
      }
      soundAt = at;
      break;
    }
    case EventKind::ShieldBroken:
      addEffect(ps,
                makeEffect(EffectKind::ShieldShatter, slot, bodyAt(player), kShieldShatterSeconds));
      flashPlayer(ps, e.player, kHitFlashSeconds);
      addShake(ps, kShakeOnHit);
      soundAt = player;
      break;
    case EventKind::BoomerangGrounded: {
      const std::optional<Vec2f> at = bananaPlane(ps, prev, curr, e.entity);
      if (at) {
        addEffect(ps,
                  makeEffect(EffectKind::GroundDust, kNoSlot, groundAt(*at), kGroundDustSeconds));
      }
      soundAt = at;
      break;
    }
    case EventKind::BoomerangBounced: {
      const std::optional<Vec2f> at = bananaPlane(ps, prev, curr, e.entity);
      if (at) {
        addEffect(ps, makeEffect(EffectKind::BounceSpark, kNoSlot,
                                 simToWorld(*at, kBananaHandHeight), kBounceSparkSeconds));
      }
      soundAt = at;
      break;
    }
    case EventKind::RoundStarted:
      addEffect(
          ps, makeEffect(EffectKind::RoundStartBanner, kNoSlot, Vec3{}, kRoundStartBannerSeconds));
      break;
    case EventKind::RoundEnded:
      addEffect(ps, makeEffect(EffectKind::RoundEndBanner, slot, Vec3{}, kRoundEndBannerSeconds));
      break;
    case EventKind::MatchEnded:
      addEffect(ps, makeEffect(EffectKind::MatchEndBanner, slot, Vec3{}, kMatchEndBannerSeconds));
      break;
    case EventKind::PlayerRespawned: {
      // At the respawned monkey (curr), which is where the sim put it.
      Vec2f at = player;
      if (const Player* p = curr.playerBySlot(e.player)) at = toFloat(p->transform.pos);
      addEffect(ps, makeEffect(EffectKind::RespawnBurst, slot, groundAt(at), kRespawnBurstSeconds));
      soundAt = at;
      break;
    }
    case EventKind::ScoreChanged:
      addEffect(ps, makeEffect(EffectKind::ScorePop, slot, groundAt(player), kScorePopSeconds));
      soundAt = player;
      break;
    case EventKind::TimerWarning:
      addEffect(ps, makeEffect(EffectKind::TimerBanner, kNoSlot, Vec3{}, kTimerBannerSeconds));
      break;
    case EventKind::PlayerBlinked: {
      // a/b: the origin's raw Q16.16 coordinates; the landing is in curr.
      const Vec2f from = toFloat(Vec2{Fixed::fromRaw(static_cast<int32_t>(e.a)),
                                      Fixed::fromRaw(static_cast<int32_t>(e.b))});
      Vec2f to = player;
      if (const Player* p = curr.playerBySlot(e.player)) to = toFloat(p->transform.pos);
      const Vec2f dir = directionOr(to - from, Vec2f{});
      addEffect(ps,
                makeEffect(EffectKind::BlinkFlash, slot, bodyAt(from), kBlinkFlashSeconds, dir));
      addEffect(ps, makeEffect(EffectKind::BlinkFlash, slot, bodyAt(to), kBlinkFlashSeconds, dir));
      soundAt = to;
      break;
    }
    case EventKind::SlamLanded: {
      RenderEffect wave =
          makeEffect(EffectKind::SlamShockwave, slot, groundAt(player), kSlamShockwaveSeconds);
      wave.size = Fixed::fromRaw(static_cast<int32_t>(e.a)).toFloat();
      addEffect(ps, wave);
      addEffect(ps,
                makeEffect(EffectKind::GroundDust, kNoSlot, groundAt(player), kGroundDustSeconds));
      addShake(ps, kShakeOnSlam);
      soundAt = player;
      break;
    }
    case EventKind::PlayerSlammed:
      // Dust kicked up under the pushed monkey; the slam itself sounds once.
      addEffect(ps,
                makeEffect(EffectKind::GroundDust, kNoSlot, groundAt(player), kGroundDustSeconds));
      flashPlayer(ps, e.player, kHitFlashSeconds);
      break;
    case EventKind::PlayerParryStarted:
      addEffect(ps, makeEffect(EffectKind::ParryGuard, slot, bodyAt(player), kParryGuardSeconds));
      break;
    case EventKind::BoomerangParried: {
      const std::optional<Vec2f> at = bananaPlane(ps, prev, curr, e.entity);
      Vec2f dir{};
      if (const Boomerang* b = curr.boomerangs.get(e.entity)) dir = toFloat(b->transform.vel);
      addEffect(ps, makeEffect(EffectKind::ParrySpark, slot,
                               simToWorld(at.value_or(player), kBananaHandHeight),
                               kParrySparkSeconds, directionOr(dir, Vec2f{})));
      flashPlayer(ps, e.player, kParrySparkSeconds);
      addShake(ps, kShakeOnHit);
      soundAt = at.value_or(player);
      break;
    }
    default:
      break;
  }

  if (std::optional<SoundCue> cue = soundForEvent(e)) {
    if (soundAt) cue->pan = soundPan(soundAt->x, halfWidth);
    pushSound(out, *cue);
  }
}

}  // namespace kfm::presentation
