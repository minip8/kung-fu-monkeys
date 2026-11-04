// The reference bot (see reference_bot.hpp). Reads only the Observation;
// fixed-point arithmetic only; every Rng draw happens in a fixed order per
// decision, so the bot is a pure function of its seed and observations.
#include "kfm/ai/reference_bot.hpp"

#include "bot_math.hpp"
#include "kfm/sim/input.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace kfm::ai {
namespace {

using botmath::angleOf;
using botmath::directionTo;
using botmath::segmentMinSq;
using botmath::squareRaw;
using botmath::unitOf;
using botmath::vec;

// Tuning (arena units, ticks). Tests pin the behaviour these give, not the numbers.
constexpr Fixed kHitSafety = Fixed::fromRaw(Fixed::kOneRaw / 8);    // margin on a contact
constexpr Fixed kCatchMargin = Fixed::fromRaw(Fixed::kOneRaw / 8);  // a catch must clearly connect
constexpr Fixed kNearMiss = Fixed::one();             // closer than contact + this is penalised
constexpr Fixed kHitPenalty = Fixed::fromInt(1000);   // a predicted contact
constexpr Fixed kHitThreshold = Fixed::fromInt(500);  // danger at or above: some path hits
constexpr Fixed kPreferredRange = Fixed::fromInt(6);  // armed: distance kept to the target
constexpr Fixed kFleeRange = Fixed::fromInt(7);       // unarmed: no gain beyond this distance
constexpr Fixed kPowerUpRadius = Fixed::fromInt(10);  // power-ups worth a detour
constexpr Fixed kWallMargin = Fixed::fromInt(2);      // outer walls: penalised closer than this
constexpr Fixed kHysteresis = Fixed::fromRaw(Fixed::kOneRaw / 4);  // keep the last direction
constexpr uint8_t kGoalTick = 16;      // where the goal is measured on a path
constexpr uint8_t kStrikeTick = 8;     // where the distance to dash-ready enemies is measured
constexpr uint8_t kDirections = 16;    // stick directions considered
constexpr uint32_t kMaxThrowHold = 2;  // a standoff's random hold before a lined-up throw

[[nodiscard]] bool ready(const ObsAbility& a, uint8_t blockedButtons) {
  return a.kind != static_cast<uint8_t>(ObsAbilityKind::None) &&
         a.phase == static_cast<uint8_t>(ObsAbilityPhase::Idle) && a.cooldownTicks == 0 &&
         (blockedButtons & a.button) == 0;
}

[[nodiscard]] Vec2 posOf(const ObsPlayer& p) {
  return vec(p.posX, p.posY);
}
[[nodiscard]] Vec2 velOf(const ObsPlayer& p) {
  return vec(p.velX, p.velY);
}

[[nodiscard]] bool hasTag(uint32_t tags, uint32_t bit) {
  return (tags & (1u << bit)) != 0;
}
constexpr uint32_t kTagInvulnerable = 4;  // sim::Tag::Invulnerable
constexpr uint32_t kTagShielded = 3;      // sim::Tag::Shielded

[[nodiscard]] Fixed distanceBetween(Vec2 a, Vec2 b) {
  return length(b - a);
}

// Whether enemy `p` can use ability slot `slot` of kind `kind` within `ticks`
// ticks (idle and the cooldown running out by then).
[[nodiscard]] bool enemyReady(const ObsPlayer& p, uint32_t slot, ObsAbilityKind kind,
                              uint32_t ticks) {
  return p.abilityKind[slot] == static_cast<uint8_t>(kind) &&
         p.abilityPhase[slot] == static_cast<uint8_t>(ObsAbilityPhase::Idle) &&
         p.abilityCooldownTicks[slot] <= ticks;
}

// Ties are broken by the bot's own Rng, never by slot, banana or direction
// index: an index order is a fixed spatial or slot preference that two
// mirrored bots share. Reservoir sampling over the candidates that tie with
// the best: the k-th tie replaces the pick with chance 1/k, so every tied
// candidate is equally likely and the Rng is drawn only on a tie (a decision
// without ties draws nothing).
struct TieBreak {
  uint32_t ties{0};
  // Whether a candidate that ties with the current best replaces it.
  [[nodiscard]] bool replaces(Rng& rng) {
    ++ties;
    return rng.below(ties) == 0;
  }
  void reset() { ties = 1; }
};

// The nearest living enemy, preferring one without spawn protection; -1 when
// none. Equal distances: a random one of them.
[[nodiscard]] int pickTarget(const Observation& obs, Rng& rng) {
  const Vec2 me = vec(obs.self.posX, obs.self.posY);
  int best = -1;
  bool bestProtected = true;
  uint64_t bestD = UINT64_MAX;
  TieBreak tie;
  for (uint32_t i = 0; i < kObsMaxPlayers; ++i) {
    const ObsPlayer& p = obs.players[i];
    if (p.alive == 0 || p.relation != static_cast<uint8_t>(ObsRelation::Enemy)) continue;
    const bool prot = hasTag(p.tags, kTagInvulnerable);
    const uint64_t d = lengthSqRaw(posOf(p) - me);
    bool take = (bestProtected && !prot) || (prot == bestProtected && d < bestD);
    if (take) {
      tie.reset();
    } else if (prot == bestProtected && d == bestD) {
      take = tie.replaces(rng);
    }
    if (take) {
      best = static_cast<int>(i);
      bestD = d;
      bestProtected = prot;
    }
  }
  return best;
}

// One tick of the own monkey: the outer box shrunk by the radius, and a
// sliding stop against the interior walls.
[[nodiscard]] Vec2 stepMonkey(const Observation& obs, Vec2 from, Vec2 vel) {
  const Fixed r = Fixed::fromRaw(obs.self.radius);
  const Vec2 lim = vec(obs.arena.halfExtents) - Vec2{r, r};
  const auto clampBox = [&](Vec2 p) {
    return Vec2{clamp(p.x, -lim.x, lim.x), clamp(p.y, -lim.y, lim.y)};
  };
  const auto blocked = [&](Vec2 p) {
    for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
      if (botmath::circleOverlaps(obs.arena.walls[w], p, r)) return true;
    }
    return false;
  };
  const Vec2 next = clampBox(from + vel);
  if (!blocked(next)) return next;
  const Vec2 xOnly = clampBox(Vec2{from.x + vel.x, from.y});
  if (!blocked(xOnly)) return xOnly;
  const Vec2 yOnly = clampBox(Vec2{from.x, from.y + vel.y});
  if (!blocked(yOnly)) return yOnly;
  return from;
}

}  // namespace

ReferenceBot::ReferenceBot(const ReferenceBotConfig& config) : config_(config) {
  config_.reactionDelayTicks = std::min(config_.reactionDelayTicks, kMaxReactionDelayTicks);
}

void ReferenceBot::init(uint64_t seed, PlayerId slot) {
  rng_ = Rng::seeded(seed ^ (uint64_t{slot.value} << 56));
  slot_ = slot;
  lastDecidedButtons_ = 0;
  lastMoveDir_ = 0;
  decisions_ = 0;
  throwHold_ = -1;
  pending_.fill(sim::PlayerInput{});
}

std::optional<sim::PlayerInput> ReferenceBot::act(const Observation& obs, Tick /*tick*/) {
  if (obs.header.version != kObservationVersion || obs.header.size != sizeof(Observation)) {
    return sim::PlayerInput{};
  }
  sim::PlayerInput in = decide(obs);
  // A press is a fresh edge: a button set in the previous decision is released.
  in.buttons = static_cast<uint8_t>(in.buttons & ~lastDecidedButtons_);
  lastDecidedButtons_ = in.buttons;
  if (in.moveMag != 0) lastMoveDir_ = in.moveDir;

  const uint32_t line = uint32_t{config_.reactionDelayTicks} + 1;
  pending_[decisions_ % line] = in;
  sim::PlayerInput out{};
  if (decisions_ >= config_.reactionDelayTicks) {
    out = pending_[(decisions_ - config_.reactionDelayTicks) % line];
  }
  ++decisions_;
  return out;
}

void ReferenceBot::predictBanana(const Observation& obs, const ObsBanana& b, Track& t) {
  const ObsRules& rules = obs.rules;
  Vec2 pos = vec(b.posX, b.posY);
  Vec2 vel = vec(b.velX, b.velY);
  Angle heading = Angle::fromBam(b.headingBam);
  auto phase = static_cast<ObsBananaPhase>(b.phase);
  uint16_t inPhase = b.ticksInPhase;
  const Fixed speed = Fixed::fromRaw(b.speed);
  const Fixed restitution = Fixed::fromRaw(rules.bananaRestitution);
  const Vec2 lim = vec(obs.arena.halfExtents) - Vec2{t.radius, t.radius};
  const bool ownerAlive = b.owner < kObsMaxPlayers && obs.players[b.owner].alive != 0;
  const Vec2 ownerPos = ownerAlive ? posOf(obs.players[b.owner]) : Vec2{};
  t.pos[0] = pos;
  t.harmlessFrom = kHorizon + 1;
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    if (phase != ObsBananaPhase::Grounded) {
      if (inPhase < UINT16_MAX) ++inPhase;
      if (phase == ObsBananaPhase::Outbound && inPhase >= rules.bananaOutboundTicks) {
        phase = ObsBananaPhase::Returning;
        inPhase = 0;
      }
      if (phase == ObsBananaPhase::Returning) {
        if (!ownerAlive || inPhase > rules.bananaMaxReturnTicks) {
          phase = ObsBananaPhase::Grounded;
          vel = Vec2{};
          if (t.harmlessFrom > kHorizon) t.harmlessFrom = k;
        } else {
          if (ownerPos != pos) {
            const int32_t rate = rules.bananaTurnRateBam;
            const int32_t delta =
                std::clamp<int32_t>(angleOf(ownerPos - pos).signedDeltaFrom(heading), -rate, rate);
            heading += Angle::fromBam(static_cast<uint16_t>(delta));
          }
          vel = Vec2::fromAngle(heading, speed);
        }
      }
      pos += vel;
      // The outer box mirrors the overshoot (interior walls are not predicted).
      bool bounced = false;
      if (pos.x > lim.x || pos.x < -lim.x) {
        const Fixed edge = pos.x > lim.x ? lim.x : -lim.x;
        pos.x = edge + edge - pos.x;
        vel.x = -vel.x * restitution;
        bounced = true;
      }
      if (pos.y > lim.y || pos.y < -lim.y) {
        const Fixed edge = pos.y > lim.y ? lim.y : -lim.y;
        pos.y = edge + edge - pos.y;
        vel.y = -vel.y * restitution;
        bounced = true;
      }
      if (bounced && vel != Vec2{}) heading = angleOf(vel);
    }
    t.pos[k] = pos;
  }
}

void ReferenceBot::trackThreats(const Observation& obs) {
  // Harmful bananas: in flight and not the own (an own outbound banana flies
  // through its thrower, an own returning one is caught on contact). The
  // nearest kMaxTracks win the slots.
  const Vec2 me = vec(obs.self.posX, obs.self.posY);
  trackCount_ = 0;
  std::array<uint64_t, kMaxTracks> dist{};
  const auto add = [&](uint64_t d) -> Track* {
    if (trackCount_ < kMaxTracks) {
      dist[trackCount_] = d;
      return &tracks_[trackCount_++];
    }
    uint8_t worst = 0;
    for (uint8_t i = 1; i < kMaxTracks; ++i) {
      if (dist[i] > dist[worst]) worst = i;
    }
    if (d >= dist[worst]) return nullptr;
    dist[worst] = d;
    return &tracks_[worst];
  };
  for (const ObsBanana& b : obs.bananas) {
    if (b.valid == 0 || b.phase == static_cast<uint8_t>(ObsBananaPhase::Grounded) ||
        b.ownerRelation == static_cast<uint8_t>(ObsRelation::Self)) {
      continue;
    }
    const uint64_t d = lengthSqRaw(vec(b.posX, b.posY) - me);
    Track* t = add(d);
    if (t == nullptr) continue;
    t->radius = Fixed::fromRaw(b.radius);
    t->catchable = true;
    predictBanana(obs, b, *t);
  }
  // A dashing enemy holding the ammo that arms a dash attack strikes on contact.
  for (const ObsPlayer& p : obs.players) {
    if (p.alive == 0 || p.relation != static_cast<uint8_t>(ObsRelation::Enemy) || p.dashing == 0 ||
        p.ammo < obs.rules.dashAttackMinAmmo) {
      continue;
    }
    Track* t = add(lengthSqRaw(posOf(p) - me));
    if (t == nullptr) continue;
    t->radius = Fixed::fromRaw(obs.rules.playerRadius);
    t->catchable = false;
    const uint16_t active = p.abilityPhase[0] == static_cast<uint8_t>(ObsAbilityPhase::Active)
                                ? p.abilityPhaseTicks[0]
                                : 0;
    const uint16_t dashTicks = obs.self.abilities[0].defActiveTicks;  // the same loadout as ours
    const uint32_t left = dashTicks > active ? uint32_t{dashTicks} - active : 0;
    t->harmlessFrom = static_cast<uint8_t>(std::min<uint32_t>(left + 1, kHorizon + 1));
    Vec2 pos = posOf(p);
    t->pos[0] = pos;
    for (uint8_t k = 1; k <= kHorizon; ++k) {
      if (k <= left) pos += velOf(p);
      t->pos[k] = pos;
    }
  }
}

// The paths: ticks 1..lead keep the current velocity (inputs already on their
// way), the plan's motion starts after.
void ReferenceBot::walkPath(const Observation& obs, uint8_t dir, bool idle, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Vec2 v = idle ? Vec2{} : unitOf(dir) * Fixed::fromRaw(obs.self.moveSpeed);
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], k <= lead ? cur : v);
  }
}

void ReferenceBot::dashPath(const Observation& obs, uint8_t dir, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const ObsAbility& dash = obs.self.abilities[0];
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Vec2 v = unitOf(dir) * Fixed::fromRaw(dash.defSpeed);
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    Vec2 step{};
    if (k <= lead) {
      step = cur;
    } else if (k <= lead + dash.defActiveTicks) {
      step = v;
    }
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], step);
  }
}

void ReferenceBot::blinkPath(const Observation& obs, uint8_t dir, Path& p) {
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const ObsAbility& blink = obs.self.abilities[2];
  const Vec2 cur = vec(obs.self.velX, obs.self.velY);
  const Vec2 walk = unitOf(dir) * Fixed::fromRaw(obs.self.moveSpeed);
  const Vec2 jump = unitOf(dir) * (Fixed::fromRaw(blink.defSpeed) * int32_t{blink.defActiveTicks});
  const uint32_t jumpAt = uint32_t{lead} + blink.defStartupTicks + 1;
  p.pos[0] = vec(obs.self.posX, obs.self.posY);
  for (uint8_t k = 1; k <= kHorizon; ++k) {
    Vec2 step{};
    if (k <= lead) {
      step = cur;
    } else if (k < jumpAt) {
      step = walk;
    } else if (k == jumpAt) {
      step = jump;  // the jump passes everything on its way (§5.5)
    }
    p.pos[k] = stepMonkey(obs, p.pos[k - 1], step);
  }
}

ReferenceBot::Outcome ReferenceBot::evaluate(const Observation& obs, const Path& p,
                                             uint8_t catchFrom, uint8_t catchUntil) const {
  Outcome o;
  const Fixed myRadius = Fixed::fromRaw(obs.self.radius);
  for (uint8_t i = 0; i < trackCount_; ++i) {
    const Track& t = tracks_[i];
    const Fixed contact = myRadius + t.radius;
    const uint64_t hitSq = squareRaw(contact + kHitSafety);
    const uint64_t catchSq = squareRaw(contact - kCatchMargin);
    const uint64_t nearSq = squareRaw(contact + kNearMiss);
    for (uint8_t k = 1; k <= kHorizon && k < t.harmlessFrom; ++k) {
      const Vec2 r = t.pos[k - 1] - p.pos[k - 1];
      const Vec2 d = (t.pos[k] - t.pos[k - 1]) - (p.pos[k] - p.pos[k - 1]);
      const uint64_t sq = segmentMinSq(r, d);
      if (t.catchable && k >= catchFrom && k < catchUntil) {
        // Inside the perfect window a contact is a catch: the banana is gone.
        if (sq < catchSq) {
          o.catches = true;
          break;
        }
        continue;
      }
      if (sq < hitSq) {
        // Sooner is worse: later predictions are less certain.
        o.danger += kHitPenalty + Fixed::fromInt(16 * (kHorizon - k));
        if (o.firstHit == 0 || k < o.firstHit) o.firstHit = k;
        break;
      }
      if (sq < nearSq) {
        const Fixed dist = Fixed::fromRaw(static_cast<int32_t>(isqrt(sq)));
        o.danger += (contact + kNearMiss - dist) * 2;
      }
    }
  }
  return o;
}

sim::PlayerInput ReferenceBot::decide(const Observation& obs) {
  sim::PlayerInput in{};
  const ObsSelf& me = obs.self;
  const Vec2 myPos = vec(me.posX, me.posY);
  const int target = pickTarget(obs, rng_);
  in.aimDir = me.facing;
  if (target >= 0) in.aimDir = directionTo(myPos, posOf(obs.players[target]));
  if (me.alive == 0 || obs.match.phase != static_cast<uint8_t>(ObsMatchPhase::Playing)) {
    throwHold_ = -1;
    return in;
  }

  trackThreats(obs);
  const uint8_t lead = std::min<uint8_t>(obs.match.inputDelayTicks, kHorizon / 2);
  const bool armed = me.ammo > 0;

  // --- movement: the best of 16 directions and idle -------------------------
  // Goal points, measured kGoalTick ticks into each path.
  const ObsPlayer* tgt = target >= 0 ? &obs.players[target] : nullptr;
  const Vec2 tgtAt = tgt != nullptr ? posOf(*tgt) + (velOf(*tgt) * int32_t{kGoalTick}) : Vec2{};
  bool haveFetch = false;
  Vec2 fetch{};
  Fixed fetchWeight = Fixed::one();
  if (!armed) {
    // The own banana on its way back, else the nearest banana lying around.
    uint64_t best = UINT64_MAX;
    for (const ObsBanana& b : obs.bananas) {
      if (b.valid == 0 || b.ownerRelation != static_cast<uint8_t>(ObsRelation::Self) ||
          b.phase == static_cast<uint8_t>(ObsBananaPhase::Grounded)) {
        continue;
      }
      const uint64_t d = lengthSqRaw(vec(b.posX, b.posY) - myPos);
      if (d < best) {
        best = d;
        fetch = vec(b.posX, b.posY);
        haveFetch = true;
      }
    }
    if (!haveFetch) {
      for (const ObsPickup& k : obs.pickups) {
        if (k.valid == 0 || k.kind != static_cast<uint8_t>(ObsPickupKind::Banana)) continue;
        const uint64_t d = lengthSqRaw(vec(k.posX, k.posY) - myPos);
        if (d < best) {
          best = d;
          fetch = vec(k.posX, k.posY);
          haveFetch = true;
          fetchWeight = Fixed::fromRatio(3, 2);
        }
      }
    }
  }
  bool havePowerUp = false;
  Vec2 powerUp{};
  {
    uint64_t best = squareRaw(kPowerUpRadius);
    for (const ObsPickup& k : obs.pickups) {
      if (k.valid == 0 || k.kind != static_cast<uint8_t>(ObsPickupKind::PowerUp)) continue;
      const uint64_t d = lengthSqRaw(vec(k.posX, k.posY) - myPos);
      if (d < best) {
        best = d;
        powerUp = vec(k.posX, k.posY);
        havePowerUp = true;
      }
    }
  }
  // Armed enemies whose dash is ready: keep out of a dash attack's reach.
  const ObsAbility& myDash = me.abilities[0];
  const Fixed strikeReach = (Fixed::fromRaw(myDash.defSpeed) * int32_t{myDash.defActiveTicks}) +
                            (Fixed::fromRaw(obs.rules.playerRadius) * 2);
  const auto strikeDanger = [&](Vec2 at, uint8_t k) {
    Fixed u{};
    for (const ObsPlayer& e : obs.players) {
      if (e.alive == 0 || e.relation != static_cast<uint8_t>(ObsRelation::Enemy) ||
          e.ammo < obs.rules.dashAttackMinAmmo || e.ammo == 0 ||
          e.abilityKind[0] != static_cast<uint8_t>(ObsAbilityKind::Dash) ||
          e.abilityCooldownTicks[0] > k) {
        continue;
      }
      const Fixed d = distanceBetween(at, posOf(e) + (velOf(e) * int32_t{k}));
      if (d < strikeReach) u += (strikeReach - d) * 2;
    }
    return u;
  };
  const Vec2 half = vec(obs.arena.halfExtents);
  const auto goal = [&](const Path& p) {
    const Vec2 end = p.pos[kGoalTick];
    Fixed u{};
    if (tgt != nullptr) {
      const Fixed d = distanceBetween(end, tgtAt);
      u -= armed ? abs(d - kPreferredRange) : -min(d, kFleeRange) / 2;
    }
    if (haveFetch) u -= distanceBetween(end, fetch) * fetchWeight;
    if (havePowerUp) u -= distanceBetween(end, powerUp) / 2;
    const Fixed ex = abs(end.x) - (half.x - kWallMargin);
    const Fixed ey = abs(end.y) - (half.y - kWallMargin);
    if (ex > Fixed{}) u -= ex * 3;
    if (ey > Fixed{}) u -= ey * 3;
    u -= strikeDanger(p.pos[kStrikeTick], kStrikeTick);
    return u;
  };

  Fixed bestUtil = Fixed::min();
  Fixed bestDanger{};
  uint8_t bestFirstHit = 0;
  uint8_t bestDir = lastMoveDir_;
  bool bestIdle = true;
  TieBreak moveTie;
  for (uint8_t c = 0; c <= kDirections; ++c) {
    const bool idle = c == kDirections;
    const auto dir = static_cast<uint8_t>(idle ? 0 : c * (256 / kDirections));
    walkPath(obs, dir, idle, path_);
    const Outcome o = evaluate(obs, path_, 0, 0);
    Fixed util = goal(path_) - o.danger;
    if (!idle && dir == lastMoveDir_) util += kHysteresis;
    bool take = util > bestUtil;
    if (take) {
      moveTie.reset();
    } else if (util == bestUtil) {
      take = moveTie.replaces(rng_);
    }
    if (take) {
      bestUtil = util;
      bestDanger = o.danger;
      bestFirstHit = o.firstHit;
      bestDir = dir;
      bestIdle = idle;
    }
  }
  in.moveDir = bestDir;
  in.moveMag = bestIdle ? 0 : 255;

  const uint8_t blocked = lastDecidedButtons_;
  const ObsAbility& dash = me.abilities[0];
  const ObsAbility& thr = me.abilities[1];
  const ObsAbility& extra = me.abilities[2];
  const bool dashReady =
      ready(dash, blocked) && dash.kind == static_cast<uint8_t>(ObsAbilityKind::Dash);
  const bool canCatch = me.ammo < me.ammoMax;
  const auto press = [&](const ObsAbility& a, uint8_t dir) {
    in.buttons = static_cast<uint8_t>(in.buttons | a.button);
    in.moveDir = dir;
    in.moveMag = 255;
  };
  bool dashed = false;

  // --- dash-catch -----------------------------------------------------------
  const auto catchFrom = static_cast<uint8_t>(lead + 1);
  const auto catchUntil = static_cast<uint8_t>(lead + 1 + dash.defPerfectWindowTicks);
  if (dashReady && canCatch && dash.defPerfectWindowTicks > 0) {
    bool found = false;
    uint8_t catchDir = 0;
    Fixed catchDanger = Fixed::max();
    for (uint8_t i = 0; i < trackCount_; ++i) {
      const Track& t = tracks_[i];
      if (!t.catchable) continue;
      for (uint8_t aim = catchFrom; aim < catchUntil && aim <= kHorizon; ++aim) {
        const uint8_t dir = directionTo(myPos, t.pos[aim]);
        dashPath(obs, dir, path_);
        const Outcome o = evaluate(obs, path_, catchFrom, catchUntil);
        if (o.catches && o.danger < catchDanger) {
          found = true;
          catchDir = dir;
          catchDanger = o.danger;
        }
      }
    }
    if (found && catchDanger < kHitThreshold && rng_.below(256) < config_.catchPer256) {
      press(dash, catchDir);
      dashed = true;
    }
  }

  // --- dodge when no walk escapes -------------------------------------------
  if (!dashed && bestDanger >= kHitThreshold) {
    if (dashReady) {
      Fixed best = bestDanger;
      bool found = false;
      uint8_t bestDash = 0;
      for (uint8_t c = 0; c < kDirections; ++c) {
        const auto dir = static_cast<uint8_t>(c * (256 / kDirections));
        dashPath(obs, dir, path_);
        const Outcome o = evaluate(obs, path_, canCatch ? catchFrom : 0, canCatch ? catchUntil : 0);
        if (o.danger < best) {
          best = o.danger;
          bestDash = dir;
          found = true;
        }
      }
      if (found) {
        press(dash, bestDash);
        dashed = true;
      }
    } else if (ready(extra, blocked)) {
      const auto kind = static_cast<ObsAbilityKind>(extra.kind);
      const uint32_t startup = extra.defStartupTicks;
      if (kind == ObsAbilityKind::Blink) {
        Fixed best = bestDanger;
        bool found = false;
        uint8_t bestBlink = 0;
        for (uint8_t c = 0; c < kDirections; ++c) {
          const auto dir = static_cast<uint8_t>(c * (256 / kDirections));
          blinkPath(obs, dir, path_);
          const Outcome o = evaluate(obs, path_, 0, 0);
          if (o.danger < best) {
            best = o.danger;
            bestBlink = dir;
            found = true;
          }
        }
        if (found) press(extra, bestBlink);
      } else if (kind == ObsAbilityKind::Parry && bestFirstHit > lead) {
        // The hit lands inside the guard's window from a press now.
        const uint32_t at = bestFirstHit - 1u - lead;
        if (at >= startup && at < startup + extra.defActiveTicks) {
          in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
        }
      } else if (kind == ObsAbilityKind::Slam && bestFirstHit > lead + startup + 1) {
        // The shockwave comes before the hit and reaches the banana.
        walkPath(obs, bestDir, bestIdle, path_);
        const uint32_t impact = lead + startup + 1;
        const Fixed reach = Fixed::fromRaw(extra.defParam0);
        for (uint8_t i = 0; i < trackCount_; ++i) {
          if (tracks_[i].catchable &&
              lengthSqRaw(tracks_[i].pos[impact] - path_.pos[impact]) < squareRaw(reach)) {
            in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
            break;
          }
        }
      }
    }
  }

  // --- dash attack ----------------------------------------------------------
  // Only on a target that cannot answer with a dash attack of its own on the
  // same tick: two strikes pressed on one tick at the same sub-tick are
  // resolved in slot order (§5.11), a coin the sim tosses by slot index, not
  // a fight the bot can win. Unarmed targets and targets whose dash is busy
  // or cooling down are fair game.
  const bool counterable = tgt != nullptr && tgt->ammo >= obs.rules.dashAttackMinAmmo &&
                           tgt->ammo > 0 && enemyReady(*tgt, 0, ObsAbilityKind::Dash, lead);
  if (!dashed && dashReady && tgt != nullptr && !counterable &&
      me.ammo >= obs.rules.dashAttackMinAmmo && me.ammo > 0 && bestDanger < kHitThreshold &&
      !hasTag(tgt->tags, kTagInvulnerable) && !hasTag(tgt->tags, kTagShielded)) {
    const Fixed strikeRange = (Fixed::fromRaw(obs.rules.playerRadius) * 2) - kCatchMargin;
    const uint64_t strikeSq = squareRaw(strikeRange);
    const Vec2 tp = posOf(*tgt);
    const Vec2 tv = velOf(*tgt);
    const uint8_t active =
        static_cast<uint8_t>(std::min<uint32_t>(dash.defActiveTicks, uint32_t{kHorizon} - lead));
    bool found = false;
    uint8_t strikeDir = 0;
    for (uint8_t aim = 1; aim <= active && !found; ++aim) {
      const uint8_t dir = directionTo(myPos, tp + (tv * int32_t{lead + aim}));
      dashPath(obs, dir, path_);
      for (uint32_t k = uint32_t{lead} + 1; k <= uint32_t{lead} + active; ++k) {
        const Vec2 e0 = tp + (tv * static_cast<int32_t>(k - 1));
        const Vec2 r = e0 - path_.pos[k - 1];
        const Vec2 d = tv - (path_.pos[k] - path_.pos[k - 1]);
        if (segmentMinSq(r, d) < strikeSq) {
          found = evaluate(obs, path_, 0, 0).danger < kHitThreshold;
          strikeDir = dir;
          break;
        }
      }
    }
    if (found && rng_.below(256) < config_.throwPer256) press(dash, strikeDir);
  }

  // --- throw ----------------------------------------------------------------
  if (tgt == nullptr || !armed || !ready(thr, blocked) ||
      thr.kind != static_cast<uint8_t>(ObsAbilityKind::Throw)) {
    throwHold_ = -1;
  } else {
    const ObsRules& r = obs.rules;
    const Fixed speed = Fixed::fromRaw(r.bananaSpeed);
    const Fixed offset = Fixed::fromRaw(r.bananaSpawnOffset);
    const int32_t windUp = int32_t{lead} + thr.defStartupTicks;
    const Vec2 from = myPos + (vec(me.velX, me.velY) * windUp);
    const Vec2 tp = posOf(*tgt);
    const Vec2 tv = velOf(*tgt);
    Vec2 aimAt = tp;
    for (int it = 0; it < 4; ++it) {
      const Fixed flight = max(distanceBetween(from, aimAt) - offset, Fixed{}) / speed;
      aimAt = tp + (tv * (flight + Fixed::fromInt(windUp)));
    }
    const Fixed range = (speed * int32_t{r.bananaOutboundTicks}) + offset - Fixed::half();
    const Fixed dist = distanceBetween(from, aimAt);
    bool clear = dist <= range;
    if (clear && obs.arena.wallCount > 0) {
      // Sample the line every half unit against the walls.
      const int32_t samples = std::max<int32_t>(1, (dist * 2).ceilToInt());
      const Fixed bananaRadius = Fixed::fromRaw(r.bananaRadius);
      for (int32_t s = 1; s <= samples && clear; ++s) {
        const Vec2 pt = from + ((aimAt - from) * Fixed::fromRatio(s, samples));
        for (uint32_t w = 0; w < obs.arena.wallCount && w < kObsMaxWalls; ++w) {
          if (botmath::circleOverlaps(obs.arena.walls[w], pt, bananaRadius)) {
            clear = false;
            break;
          }
        }
      }
    }
    // A standoff (the target holds a banana and could throw it on this very
    // tick): two bots that line up on the same tick, as mirrored monkeys do
    // at a round's start, would trade and both fall on one tick. A hold of
    // 0..kMaxThrowHold ticks, drawn once per lined-up shot from the bot's own
    // Rng, breaks the mirror without a fixed preference; outside a standoff
    // the shot goes at once.
    if (!clear) {
      throwHold_ = -1;
    } else if (throwHold_ < 0) {
      const bool standoff = tgt->ammo > 0 && enemyReady(*tgt, 1, ObsAbilityKind::Throw, lead);
      throwHold_ = standoff ? static_cast<int8_t>(rng_.below(kMaxThrowHold + 1)) : 0;
    }
    if (clear && throwHold_ > 0) {
      --throwHold_;
      in.aimDir = directionTo(myPos, aimAt);
    } else if (clear && rng_.below(256) < config_.throwPer256) {
      throwHold_ = -1;
      in.buttons = static_cast<uint8_t>(in.buttons | thr.button);
      int32_t aim = directionTo(myPos, aimAt);
      if (config_.aimNoise != 0) aim += rng_.range(-int32_t{config_.aimNoise}, config_.aimNoise);
      in.aimDir = static_cast<uint8_t>(aim & 0xFF);
    } else {
      in.aimDir = directionTo(myPos, aimAt);
    }
  }

  // --- slam on a monkey in reach ---------------------------------------------
  if (tgt != nullptr && ready(extra, blocked) && (in.buttons & extra.button) == 0 &&
      extra.kind == static_cast<uint8_t>(ObsAbilityKind::Slam)) {
    const uint32_t impact =
        std::min<uint32_t>(uint32_t{lead} + extra.defStartupTicks + 1u, kHorizon);
    const Vec2 at = posOf(*tgt) + (velOf(*tgt) * static_cast<int32_t>(impact));
    walkPath(obs, in.moveDir, in.moveMag == 0, path_);
    const Fixed reach = Fixed::fromRaw(extra.defParam0) - Fixed::half();
    if (lengthSqRaw(at - path_.pos[impact]) < squareRaw(reach) &&
        rng_.below(256) < config_.throwPer256) {
      in.buttons = static_cast<uint8_t>(in.buttons | extra.button);
    }
  }
  return in;
}

}  // namespace kfm::ai
