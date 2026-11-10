#include "kfm/sim/visit.hpp"

#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>

using namespace kfm;
using namespace kfm::literals;
using namespace kfm::sim;

namespace {
WorldState sampleWorld() {
  WorldState s = initialWorld(MatchConfig{42, 0b0000001011});
  EventSink sink;
  for (int i = 0; i < 5; ++i) {
    InputFrame f;
    f.tick = s.tick;
    f.present = s.rules.present;
    f.inputs[0].moveDir = 64;
    step(s, f, defaultContent(), sink);
  }
  return s;
}
}  // namespace

TEST_CASE("checksum is a function of visited state only", "[sim][visit]") {
  const WorldState a = sampleWorld();
  WorldState b = a;
  CHECK(checksum(a) == checksum(b));

  // Garbage in a dead pool slot must not change the checksum.
  b.players.items[5].transform.pos.x = 123_fx;
  CHECK(checksum(a) == checksum(b));

  // A change in an alive slot must.
  b = a;
  b.players.items[0].transform.pos.x += Fixed::epsilon();
  CHECK(checksum(a) != checksum(b));

  b = a;
  ++b.tick;
  CHECK(checksum(a) != checksum(b));
}

TEST_CASE("write/read visitor round-trips WorldState", "[sim][visit]") {
  const WorldState a = sampleWorld();
  std::array<std::byte, 8192> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));

  WorldState b{};
  b.players.items[7].id = PlayerId{7};  // garbage the reader must canonicalise away
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(r.atEnd());

  CHECK(b.tick == a.tick);
  CHECK(b.rng == a.rng);
  CHECK(b.players.size() == a.players.size());
  CHECK(checksum(a) == checksum(b));
  CHECK(b.players.items[7].id == PlayerId::invalid());
  a.players.forEachAlive([&](EntityId id, const Player& p) {
    const Player* q = b.players.get(id);
    REQUIRE(q != nullptr);
    CHECK(q->id == p.id);
    CHECK(q->transform.pos == p.transform.pos);
    CHECK(q->transform.facing == p.transform.facing);
  });
}

TEST_CASE("InputFrame round-trip only carries present players", "[sim][visit]") {
  InputFrame f;
  f.tick = Tick{77};
  f.present = 0b101;
  f.inputs[0] = PlayerInput{1, 2, 3, 4, 5};
  f.inputs[1] = PlayerInput{9, 9, 9, 9, 9};  // absent: not encoded
  f.inputs[2] = PlayerInput{6, 7, 8, 9, 10};
  std::array<std::byte, 64> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, f));
  CHECK(w.bytesWritten() == 4 + 2 + 2 * 5);
  InputFrame g;
  BitReader r(w.written());
  REQUIRE(readFrom(r, g));
  CHECK(g.tick == f.tick);
  CHECK(g.present == f.present);
  CHECK(g.inputs[0] == f.inputs[0]);
  CHECK(g.inputs[1] == PlayerInput{});
  CHECK(g.inputs[2] == f.inputs[2]);
}

// Constant-initialised, so it lands in read-only storage: writing through
// it would fault at -O0 rather than merely being UB.
constexpr MatchRules kConstRules{MatchPhase::Playing, 0b11, 7};

TEST_CASE("checksum and write work on a const object", "[sim][visit]") {
  CHECK(checksumOf(kConstRules) == checksumOf(MatchRules{MatchPhase::Playing, 0b11, 7}));
  // Visitors that do not read back must never write through the reference.
  const WorldState world = sampleWorld();
  CHECK(checksum(world) == checksum(sampleWorld()));
  std::array<std::byte, 8192> buf{};
  BitWriter w(buf);
  CHECK(writeTo(w, world));
}

TEST_CASE("reader rejects a pool that violates its invariants", "[sim][visit]") {
  WorldState a = sampleWorld();
  std::array<std::byte, 8192> buf{};

  SECTION("stray alive bit above capacity") {
    a.players.aliveBits[0] |= uint64_t{1} << 63;
  }
  SECTION("aliveCount disagrees with the alive bits") {
    a.players.aliveCount = 0;
  }
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  CHECK_FALSE(readFrom(r, b));
  CHECK(b.players.empty());
  CHECK(b.players.invariantsHold());
}

TEST_CASE("InputFrame equality ignores absent players", "[sim][visit]") {
  InputFrame f;
  f.tick = Tick{3};
  f.present = 0b1;
  f.inputs[0].moveDir = 5;
  InputFrame g = f;
  g.inputs[4].moveDir = 200;  // absent slot
  CHECK(f == g);
  g.inputs[0].moveDir = 6;
  CHECK(f != g);
  g = f;
  g.present = 0b11;
  CHECK(f != g);
}

namespace {
// The item of a freshly spawned entity. Aborts on a full pool (never the
// case here), which also tells GCC's Release flow analysis that the index is
// in range (-Wstringop-overflow otherwise).
template <class PoolT>
auto& spawned(PoolT& pool, EntityId id) {
  if (!id.isValid() || id.index >= pool.items.size()) std::abort();
  return pool.items[id.index];
}

// A world exercising every M1 entity type and field the visitor carries.
WorldState richWorld() {
  WorldState s = sampleWorld();
  Player& p = s.players.items[0];
  p.intent.pressed = 3;
  p.tags.set(Tag::HitStun);
  p.hitStunTicks = 4;
  p.modifiers.count = 2;
  p.modifiers.items[0] = ActiveModifier{0, 30};
  p.modifiers.items[1] = ActiveModifier{3, 60};
  p.abilities[1].phase = AbilityPhase::Recovery;
  p.abilities[1].activatedTick = Tick{3};
  p.abilities[1].activatedSubTick = 99;
  p.abilities[1].dir = Vec2{1_fx, 0_fx};
  s.rules.round = 2;
  s.rules.scores[3] = 1;
  s.rules.lastRoundWinner = PlayerId{3};
  const EntityId b = s.boomerangs.spawn();
  spawned(s.boomerangs, b) =
      Boomerang{PlayerId{1},
                BoomerangPhase::Returning,
                Transform{Vec2{2_fx, 3_fx}, Vec2{1_fx, 3_fx}, Vec2{1_fx, 0_fx}, Angle::fromBam(5)},
                Angle::fromBam(77),
                40,
                4,
                1};
  const EntityId k = s.pickups.spawn();
  spawned(s.pickups, k) = Pickup{PickupKind::Banana, Vec2{-4_fx, 5_fx}, 0, 0};
  // v0.20: a power-up pickup and a banana carrying projectile modifiers.
  const EntityId k2 = s.pickups.spawn();
  spawned(s.pickups, k2) = Pickup{PickupKind::PowerUp, Vec2{6_fx, -2_fx}, 4, 2};
  const EntityId b2 = s.boomerangs.spawn();
  Boomerang& big = spawned(s.boomerangs, b2);
  big.owner = PlayerId{0};
  big.radiusMul = 2_fx;
  big.speedMul = Fixed::fromRatio(17, 20);
  return s;
}
}  // namespace

TEST_CASE("write/read round-trips every M1 state type", "[sim][visit]") {
  const WorldState a = richWorld();
  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(r.atEnd());
  CHECK(checksum(a) == checksum(b));
  CHECK(b.rules.lastRoundWinner == PlayerId{3});
  CHECK(b.rules.scores[3] == 1);
  CHECK(b.boomerangs.size() == 2);
  CHECK(b.boomerangs.items[1].radiusMul == 2_fx);
  CHECK(b.boomerangs.items[1].speedMul == Fixed::fromRatio(17, 20));
  CHECK(b.boomerangs.items[0].radiusMul == Fixed::one());
  CHECK(b.pickups.items[1].kind == PickupKind::PowerUp);
  CHECK(b.pickups.items[1].powerUpIndex == 4);
  CHECK(b.pickups.items[1].spawner == 2);
  CHECK(b.boomerangs.items[0].phase == BoomerangPhase::Returning);
  CHECK(b.boomerangs.items[0].transform.prevPos == Vec2{1_fx, 3_fx});
  CHECK(b.pickups.items[0].pos == Vec2{-4_fx, 5_fx});
  const Player& p = b.players.items[0];
  CHECK(p.modifiers.count == 2);
  CHECK(p.modifiers.items[1].powerUp == 3);
  CHECK(p.modifiers.items[1].ticksLeft == 60);
  CHECK(p.tags.has(Tag::HitStun));
  CHECK(p.abilities[1].phase == AbilityPhase::Recovery);
  CHECK(p.abilities[1].activatedSubTick == 99);
}

namespace {
// One mutation per field of T, in declaration order, each of which must move
// the checksum. The count is pinned to fieldCount<T>() like the visit()
// overloads are, so a new field of a state struct fails to compile here too
// until it has a mutation (an array or a nested struct is one field: one
// element or one member of it is enough).
using StateMutation = void (*)(WorldState&);
template <class T, size_t N>
void everyFieldReachesTheChecksum(const WorldState& base,
                                  const std::array<StateMutation, N>& mutations) {
  static_assert(N == fieldCount<T>(), "one mutation per field");
  const uint32_t before = checksum(base);
  for (size_t i = 0; i < N; ++i) {
    WorldState s = base;
    mutations[i](s);
    INFO("field " << i);
    CHECK(checksum(s) != before);
  }
}
}  // namespace

TEST_CASE("every state field reaches the checksum", "[sim][visit]") {
  const WorldState a = richWorld();
  // Slot 0 is alive, and so are boomerangs 0-1 and pickups 0-1 (richWorld).
  everyFieldReachesTheChecksum<WorldState>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { ++s.tick; },
             [](WorldState& s) { s.rng.s[2] ^= 1u; },
             [](WorldState& s) { s.rules.round += 1; },
             [](WorldState& s) { s.players.items[0].hp += 1; },
             [](WorldState& s) { s.boomerangs.items[0].damage += 1; },
             [](WorldState& s) { s.pickups.items[0].count += 1; },
         }));
  everyFieldReachesTheChecksum<Rng>(a, std::to_array<StateMutation>({
                                           [](WorldState& s) { s.rng.s[3] += 1u; },
                                       }));
  everyFieldReachesTheChecksum<Pool<Player, kMaxPlayers>>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[1].ammo += 1; },
             [](WorldState& s) { s.players.generation[9] += 1; },  // dead slots' too
             [](WorldState& s) { s.players.aliveBits[0] ^= 1u << 5; },
             [](WorldState& s) { s.players.aliveCount += 1; },
         }));
  everyFieldReachesTheChecksum<MatchRules>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.rules.phase = MatchPhase::MatchEnded; },
             [](WorldState& s) { s.rules.present ^= 1u << 9; },
             [](WorldState& s) { s.rules.ticksInPhase += 1; },
             [](WorldState& s) { s.rules.round += 1; },
             [](WorldState& s) { s.rules.scores[9] = -1; },
             [](WorldState& s) { s.rules.lastRoundWinner = PlayerId{1}; },
             [](WorldState& s) { s.rules.deaths[9] = 1; },
             [](WorldState& s) { s.rules.respawnTicks[0] = 1; },
             [](WorldState& s) { s.rules.invulnTicks[4] = 1; },
             [](WorldState& s) { s.rules.modeData[7] = 1; },
             [](WorldState& s) { s.rules.dryTicks += 1; },
         }));
  everyFieldReachesTheChecksum<Player>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].id = PlayerId{4}; },
             [](WorldState& s) { s.players.items[0].transform.vel.y += 1_fx; },
             [](WorldState& s) { s.players.items[0].intent.held ^= 1; },
             [](WorldState& s) { s.players.items[0].prevButtons ^= 1; },
             [](WorldState& s) { s.players.items[0].stats.maxHp += 1; },
             [](WorldState& s) { s.players.items[0].modifiers.count = 1; },
             [](WorldState& s) { s.players.items[0].hp += 1; },
             [](WorldState& s) { s.players.items[0].ammo += 1; },
             [](WorldState& s) { s.players.items[0].tags.set(Tag::Shielded); },
             [](WorldState& s) { s.players.items[0].hitStunTicks += 1; },
             [](WorldState& s) { s.players.items[0].grantedTags.set(Tag::Shielded); },
             [](WorldState& s) { s.players.items[0].abilities[2].cooldownTicks += 1; },
             [](WorldState& s) { s.players.items[0].knockback.y += 1_fx; },
             [](WorldState& s) { s.players.items[0].knockbackTicks += 1; },
             [](WorldState& s) { s.players.items[0].moveTarget.x += 1_fx; },
             [](WorldState& s) { s.players.items[0].moveTicks += 1; },
             [](WorldState& s) { s.players.items[0].fallTicks += 1; },
             [](WorldState& s) { s.players.items[0].regrowTicks += 1; },
         }));
  everyFieldReachesTheChecksum<Transform>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].transform.pos.x += 1_fx; },
             [](WorldState& s) { s.players.items[0].transform.prevPos.x += 1_fx; },
             [](WorldState& s) { s.players.items[0].transform.vel.x += 1_fx; },
             [](WorldState& s) { s.players.items[0].transform.facing += Angle::fromBam(1); },
         }));
  everyFieldReachesTheChecksum<Vec2>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.boomerangs.items[1].transform.pos.x += 1_fx; },
             [](WorldState& s) { s.boomerangs.items[1].transform.pos.y += 1_fx; },
         }));
  everyFieldReachesTheChecksum<Intent>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].intent.move.x += Fixed::epsilon(); },
             [](WorldState& s) { s.players.items[0].intent.aim += Angle::fromBam(1); },
             [](WorldState& s) { s.players.items[0].intent.pressed ^= 1; },
             [](WorldState& s) { s.players.items[0].intent.held ^= 2; },
             [](WorldState& s) { s.players.items[0].intent.subTick += 1; },
         }));
  everyFieldReachesTheChecksum<Stats>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].stats.moveSpeed += Fixed::epsilon(); },
             [](WorldState& s) { s.players.items[0].stats.maxHp += 1; },
             [](WorldState& s) { s.players.items[0].stats.ammoMax += 1; },
             [](WorldState& s) { s.players.items[0].stats.radius += Fixed::epsilon(); },
         }));
  // Only the live prefix (count = 2) is visited.
  everyFieldReachesTheChecksum<ModifierList>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].modifiers.items[1].ticksLeft -= 1; },
             [](WorldState& s) { s.players.items[0].modifiers.count = 3; },
         }));
  everyFieldReachesTheChecksum<ActiveModifier>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].modifiers.items[0].powerUp = 5; },
             [](WorldState& s) { s.players.items[0].modifiers.items[0].ticksLeft += 1; },
         }));
  everyFieldReachesTheChecksum<AbilityState>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.players.items[0].abilities[0].defIndex ^= 1; },
             [](WorldState& s) { s.players.items[0].abilities[0].phase = AbilityPhase::Startup; },
             [](WorldState& s) { s.players.items[0].abilities[0].phaseTicks += 1; },
             [](WorldState& s) { s.players.items[0].abilities[0].cooldownTicks = 9; },
             [](WorldState& s) { ++s.players.items[0].abilities[0].activatedTick; },
             [](WorldState& s) { s.players.items[0].abilities[0].activatedSubTick += 1; },
             [](WorldState& s) { s.players.items[0].abilities[0].dir.y += 1_fx; },
         }));
  everyFieldReachesTheChecksum<Boomerang>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.boomerangs.items[0].owner = PlayerId{2}; },
             [](WorldState& s) { s.boomerangs.items[0].phase = BoomerangPhase::Grounded; },
             [](WorldState& s) { s.boomerangs.items[0].transform.prevPos.y += 1_fx; },
             [](WorldState& s) { s.boomerangs.items[0].spin += Angle::fromBam(1); },
             [](WorldState& s) { s.boomerangs.items[0].ticksAlive += 1; },
             [](WorldState& s) { s.boomerangs.items[0].ticksInPhase += 1; },
             [](WorldState& s) { s.boomerangs.items[0].damage += 1; },
             [](WorldState& s) { s.boomerangs.items[0].speedMul = 2_fx; },
             [](WorldState& s) { s.boomerangs.items[1].radiusMul = 3_fx; },
         }));
  everyFieldReachesTheChecksum<Pickup>(
      a, std::to_array<StateMutation>({
             [](WorldState& s) { s.pickups.items[0].kind = PickupKind::Flag; },
             [](WorldState& s) { s.pickups.items[0].pos.y += 1_fx; },
             [](WorldState& s) { s.pickups.items[0].powerUpIndex = 1; },
             [](WorldState& s) { s.pickups.items[1].spawner = 3; },
             [](WorldState& s) { s.pickups.items[0].count += 1; },
         }));
}

TEST_CASE("write/read round-trips the per-player mode counters", "[sim][visit]") {
  WorldState a = richWorld();
  a.rules.deaths[2] = 3;
  a.rules.respawnTicks[2] = 40;
  a.rules.invulnTicks[7] = 9;
  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(r.atEnd());
  CHECK(checksum(b) == checksum(a));
  CHECK(b.rules.deaths[2] == 3);
  CHECK(b.rules.respawnTicks[2] == 40);
  CHECK(b.rules.invulnTicks[7] == 9);
}

TEST_CASE("ModifierList past count is canonicalised", "[sim][visit]") {
  const WorldState a = richWorld();
  WorldState garbage = a;
  garbage.players.items[0].modifiers.items[5] = ActiveModifier{7, 777};  // past count = 2
  CHECK(checksum(garbage) == checksum(a));

  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, garbage));
  WorldState b{};
  b.players.items[0].modifiers.items[6].ticksLeft = 55;  // reader must clear this too
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(b.players.items[0].modifiers.items[5].ticksLeft == 0);
  CHECK(b.players.items[0].modifiers.items[6].ticksLeft == 0);
  CHECK(checksum(b) == checksum(a));
}

TEST_CASE("reader rejects a ModifierList count above capacity", "[sim][visit]") {
  WorldState a = richWorld();
  a.players.items[0].modifiers.count = kMaxModifiers + 1;
  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  CHECK_FALSE(readFrom(r, b));
}

TEST_CASE("ContentDB round-trips through the visitor", "[sim][visit]") {
  const ContentDB& c = defaultContent();
  std::array<std::byte, sizeof(ContentDB)> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, c));  // works on the constexpr (read-only) instance
  ContentDB d{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, d));
  CHECK(r.atEnd());
  CHECK(contentHash(d) == contentHash(c));
  CHECK(checksumOf(d) == checksumOf(c));
}

// A snapshot from the network must not be able to smuggle in a state the
// systems index or shift by: each corruption below is written raw (the
// writer does not validate) and must make readFrom fail.
TEST_CASE("reader rejects out-of-range enums, ids and indices", "[sim][visit]") {
  WorldState a = richWorld();
  const auto raw = [](auto& e, uint8_t v) {
    std::memcpy(&e, &v, 1);  // an enum value outside its declared range
  };

  SECTION("MatchPhase") {
    raw(a.rules.phase, 5);
  }
  SECTION("AbilityPhase") {
    raw(a.players.items[0].abilities[0].phase, 4);
  }
  SECTION("BoomerangPhase (would shift by 40)") {
    raw(a.boomerangs.items[0].phase, 40);
  }
  SECTION("PickupKind") {
    raw(a.pickups.items[0].kind, 3);
  }
  SECTION("ActiveModifier.powerUp past the def table") {
    a.players.items[0].modifiers.items[0].powerUp = kMaxPowerUps;
  }
  SECTION("Pickup.powerUpIndex past the def table") {
    a.pickups.items[0].powerUpIndex = kMaxPowerUps;
  }
  SECTION("Pickup.spawner past the spawner table") {
    a.pickups.items[1].spawner = kMaxPickupSpawners;
  }
  SECTION("Player.grantedTags above Tag::kCount") {
    a.players.items[0].grantedTags.bits |= uint32_t{1} << static_cast<uint32_t>(Tag::kCount);
  }
  SECTION("player id differs from its slot (would index scores[200])") {
    a.players.items[0].id = PlayerId{200};
  }
  SECTION("player id is another valid slot") {
    a.players.items[0].id = PlayerId{3};
  }
  SECTION("AbilityState.defIndex past the def table") {
    a.players.items[0].abilities[0].defIndex = kMaxAbilityDefs;
  }
  SECTION("boomerang owner is neither invalid nor a slot") {
    a.boomerangs.items[0].owner = PlayerId{kMaxPlayers};
  }
  SECTION("lastRoundWinner is neither invalid nor a slot") {
    a.rules.lastRoundWinner = PlayerId{17};
  }
  SECTION("tag bits above Tag::kCount") {
    a.players.items[0].tags.bits |= uint32_t{1} << static_cast<uint32_t>(Tag::kCount);
  }

  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  CHECK_FALSE(readFrom(r, b));
}

TEST_CASE("reader accepts boundary values and keeps the checksum", "[sim][visit]") {
  WorldState a = richWorld();
  a.rules.phase = MatchPhase::MatchEnded;
  a.boomerangs.items[0].owner = PlayerId::invalid();
  a.boomerangs.items[0].phase = BoomerangPhase::Grounded;
  a.players.items[0].abilities[0].defIndex = kMaxAbilityDefs - 1;
  a.players.items[0].modifiers.items[0].powerUp = kMaxPowerUps - 1;
  a.pickups.items[1].spawner = kMaxPickupSpawners - 1;
  a.players.items[0].tags.set(Tag::Eliminated);
  a.rules.lastRoundWinner = PlayerId{kMaxPlayers - 1};
  std::array<std::byte, 16384> buf{};
  BitWriter w(buf);
  REQUIRE(writeTo(w, a));
  WorldState b{};
  BitReader r(w.written());
  REQUIRE(readFrom(r, b));
  CHECK(checksum(b) == checksum(a));
}
