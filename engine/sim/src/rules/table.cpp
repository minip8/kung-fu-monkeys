// The interaction rules table (architecture §5.6). New twist = new row
// (plus a tag or handler); rows stay sorted by priority.
#include "kfm/sim/rules/handlers.hpp"
#include "kfm/sim/rules/interaction.hpp"

#include <algorithm>
#include <array>

namespace kfm::sim {
namespace {

// A player eliminated earlier in the same tick is still in the pool until
// LifecycleSystem runs; no row may touch it again. Nor one falling down a pit
// (v0.57): bananas fly over it, nobody dashes into or pushes it, it takes
// nothing, and the pit row does not start its fall twice.
constexpr Tags kGone = Tags::of(Tag::Eliminated) | Tag::Falling;
// A slam row also matches contacts made only through the slammer's reach.
constexpr uint8_t kReach = 1;

// The perfect catch is two rows: any banana in flight that is not the
// catcher's, and the catcher's own banana only once it is returning. An own
// outbound banana passes through its thrower (as it does without a dash), so
// throwing while dashing never catches the banana just thrown. The handler
// declines when the catcher is already at ammoMax (v0.16): row 30 then hits a
// full-ammo dasher with a non-own banana, row 40 absorbs an own returning one.
// The dash attack (row 25) keys on (Player, Player) with the attacker as A;
// InteractionSystem tries each player contact both ways round. A stunned
// attacker cannot strike, so the loser of a mutual dash cannot retaliate in
// the same tick.
//
// The shield (v0.20, rows 20 and 24) sits just above the row it pre-empts
// and keys on the same tags, so it only breaks on a hit that would land.
// Pickups are split by kind: row 50 the grounded banana, row 51 a power-up,
// row 52 (v0.33) a capture flag, which a carrier cannot take a second of.
// Player × Hazard (row 60, v0.22) is the pit: a player whose centre is over
// it starts to fall (v0.57), unless it is Dashing: a dash carries a monkey
// over a pit, and it falls if its dash ends with the centre still over one.
// Every Boomerang row names a phase mask of Outbound/Returning, so a banana
// sinking into a pit (v0.57) matches none.
// The parry (v0.21, row 15) sits below the perfect catch (both reward
// timing; the catch also gains a banana at once, so it wins a tie) and above
// the slam, the shield and damage: a mistimed parry declines and falls
// through to them.
// The slam (v0.21, rows 16 and 26) is the only pair of rows that accepts a
// contact made through a slammer's widened radius (InteractionRule::reachA):
// it grounds bananas below the catch (a perfect catch still gains the banana)
// and above the shield and damage, and pushes monkeys after the dash attack.
//
// Relations (v0.32, InteractionRule::relationB). Every row that hurts,
// pushes or answers a hit applies to an Enemy, and its twin a few
// priorities later applies to an Ally only when the monkey on the receiving
// end holds Tag::FriendlyFire (a mode's baseline grants it): the victim A of
// a banana row (17, 18, 21, 31 against 15, 16, 20, 30), the victim B of a
// player pair (27, 28, 29 against 24, 25, 26). A contact is Ally or Enemy,
// never both, so each twin only competes with the rows of its own relation
// and their order among themselves is the order of the Enemy rows. The
// catch (10/11), auto-catch and the pickup and pit rows keep every bit: a
// perfect dash catches a teammate's banana (a pass) in any mode. In a
// free-for-all every other slot is an enemy and no Ally row ever matches,
// so the table plays exactly as before.
constexpr uint8_t kEnemy = relationBit(Relation::Enemy);
constexpr uint8_t kAlly = relationBit(Relation::Ally);
constexpr Tag kFf = Tag::FriendlyFire;

constexpr std::array<InteractionRule, 21> kRules = {{
    // Player[PerfectDash] x Boomerang[InFlight, !own]: perfect catch.
    {10, EntityKind::Player, Tags::of(Tag::PerfectDash), kGone, EntityKind::Boomerang, Tags{},
     Tags{}, kPhaseInFlight, Ownership::NotOwn, &catchBoomerang},
    // Player[PerfectDash] x Boomerang[Returning, own]: perfect catch of one's own.
    {11, EntityKind::Player, Tags::of(Tag::PerfectDash), kGone, EntityKind::Boomerang, Tags{},
     Tags{}, kPhaseReturning, Ownership::Own, &catchBoomerang},
    // Player[Parrying, !HitStun] x Boomerang[InFlight, !own] (Enemy): reflected (declines
    // outside the window).
    {15, EntityKind::Player, Tags::of(Tag::Parrying), kGone | Tag::HitStun, EntityKind::Boomerang,
     Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &parryBoomerang, kPickupAny, 0, kEnemy},
    // Player[Slamming, !HitStun] x Boomerang[InFlight, !own] (Enemy), through the reach:
    // grounded.
    {16, EntityKind::Player, Tags::of(Tag::Slamming), kGone | Tag::HitStun, EntityKind::Boomerang,
     Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &slamGroundsBanana, kPickupAny, kReach,
     kEnemy},
    // Player[Parrying, FriendlyFire, !HitStun] x Boomerang[InFlight, !own] (Ally).
    {17, EntityKind::Player, Tags::of(Tag::Parrying) | kFf, kGone | Tag::HitStun,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &parryBoomerang,
     kPickupAny, 0, kAlly},
    // Player[Slamming, FriendlyFire, !HitStun] x Boomerang[InFlight, !own] (Ally), reach.
    {18, EntityKind::Player, Tags::of(Tag::Slamming) | kFf, kGone | Tag::HitStun,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &slamGroundsBanana,
     kPickupAny, kReach, kAlly},
    // Player[Shielded, !HitStun, !Invulnerable] x Boomerang[InFlight, !own] (Enemy): shield
    // blocks.
    {20, EntityKind::Player, Tags::of(Tag::Shielded), kGone | Tag::HitStun | Tag::Invulnerable,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &shieldBlocksBanana,
     kPickupAny, 0, kEnemy},
    // Player[Shielded, FriendlyFire, !HitStun, !Invulnerable] x Boomerang[InFlight, !own] (Ally).
    {21, EntityKind::Player, Tags::of(Tag::Shielded) | kFf,
     kGone | Tag::HitStun | Tag::Invulnerable, EntityKind::Boomerang, Tags{}, Tags{},
     kPhaseInFlight, Ownership::NotOwn, &shieldBlocksBanana, kPickupAny, 0, kAlly},
    // Player[Dashing, !HitStun] x Player[Shielded, !HitStun, !Invulnerable] (Enemy): shield
    // blocks.
    {24, EntityKind::Player, Tags::of(Tag::Dashing), kGone | Tag::HitStun, EntityKind::Player,
     Tags::of(Tag::Shielded), kGone | Tag::HitStun | Tag::Invulnerable, kPhaseAny, Ownership::Any,
     &shieldBlocksDash, kPickupAny, 0, kEnemy},
    // Player[Dashing, !HitStun] x Player[!HitStun, !Invulnerable] (Enemy): dash attack.
    {25, EntityKind::Player, Tags::of(Tag::Dashing), kGone | Tag::HitStun, EntityKind::Player,
     Tags{}, kGone | Tag::HitStun | Tag::Invulnerable, kPhaseAny, Ownership::Any, &dashAttack,
     kPickupAny, 0, kEnemy},
    // Player[Slamming, !HitStun] x Player[] (Enemy), through the reach: pushed away.
    {26, EntityKind::Player, Tags::of(Tag::Slamming), kGone | Tag::HitStun, EntityKind::Player,
     Tags{}, kGone, kPhaseAny, Ownership::Any, &slamPushesPlayer, kPickupAny, kReach, kEnemy},
    // Player[Dashing, !HitStun] x Player[Shielded, FriendlyFire, !HitStun, !Invulnerable] (Ally).
    {27, EntityKind::Player, Tags::of(Tag::Dashing), kGone | Tag::HitStun, EntityKind::Player,
     Tags::of(Tag::Shielded) | kFf, kGone | Tag::HitStun | Tag::Invulnerable, kPhaseAny,
     Ownership::Any, &shieldBlocksDash, kPickupAny, 0, kAlly},
    // Player[Dashing, !HitStun] x Player[FriendlyFire, !HitStun, !Invulnerable] (Ally).
    {28, EntityKind::Player, Tags::of(Tag::Dashing), kGone | Tag::HitStun, EntityKind::Player,
     Tags::of(kFf), kGone | Tag::HitStun | Tag::Invulnerable, kPhaseAny, Ownership::Any,
     &dashAttack, kPickupAny, 0, kAlly},
    // Player[Slamming, !HitStun] x Player[FriendlyFire] (Ally), through the reach.
    {29, EntityKind::Player, Tags::of(Tag::Slamming), kGone | Tag::HitStun, EntityKind::Player,
     Tags::of(kFf), kGone, kPhaseAny, Ownership::Any, &slamPushesPlayer, kPickupAny, kReach, kAlly},
    // Player[!HitStun, !Invulnerable] x Boomerang[InFlight, !own] (Enemy): damage.
    {30, EntityKind::Player, Tags{}, kGone | Tag::HitStun | Tag::Invulnerable,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &damagePlayer,
     kPickupAny, 0, kEnemy},
    // Player[FriendlyFire, !HitStun, !Invulnerable] x Boomerang[InFlight, !own] (Ally): damage.
    {31, EntityKind::Player, Tags::of(kFf), kGone | Tag::HitStun | Tag::Invulnerable,
     EntityKind::Boomerang, Tags{}, Tags{}, kPhaseInFlight, Ownership::NotOwn, &damagePlayer,
     kPickupAny, 0, kAlly},
    // Player[] x Boomerang[Returning, own]: auto catch.
    {40, EntityKind::Player, Tags{}, kGone, EntityKind::Boomerang, Tags{}, Tags{}, kPhaseReturning,
     Ownership::Own, &autoCatch},
    // Player[] x Pickup[Banana]: take the banana.
    {50, EntityKind::Player, Tags{}, kGone, EntityKind::Pickup, Tags{}, Tags{}, kPhaseAny,
     Ownership::Any, &takePickup, pickupKindBit(PickupKind::Banana)},
    // Player[] x Pickup[PowerUp]: grant the power-up.
    {51, EntityKind::Player, Tags{}, kGone, EntityKind::Pickup, Tags{}, Tags{}, kPhaseAny,
     Ownership::Any, &grantPowerUp, pickupKindBit(PickupKind::PowerUp)},
    // Player[!Carrying] x Pickup[Flag] (v0.33): an enemy of the flag's team takes it.
    {52, EntityKind::Player, Tags{}, kGone | Tag::Carrying, EntityKind::Pickup, Tags{}, Tags{},
     kPhaseAny, Ownership::Any, &takeFlag, pickupKindBit(PickupKind::Flag)},
    // Player[!Dashing] x Hazard[Pit]: falls in (v0.57), eliminated by nobody when it lands.
    {60, EntityKind::Player, Tags{}, kGone | Tag::Dashing, EntityKind::Hazard, Tags{}, Tags{},
     kPhaseAny, Ownership::Any, &fallIntoPit},
}};

static_assert(std::ranges::is_sorted(kRules, {}, &InteractionRule::priority));

}  // namespace

std::span<const InteractionRule> interactionRules() {
  return kRules;
}

}  // namespace kfm::sim
