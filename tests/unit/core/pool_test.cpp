#include "kfm/core/pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

using kfm::EntityId;
using kfm::Pool;

namespace {
struct Thing {
  uint16_t value{0};
  uint16_t other{42};
};
}  // namespace

TEST_CASE("Pool is trivially copyable and starts empty", "[core][pool]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<Pool<Thing, 8>>);
  const Pool<Thing, 8> p{};
  CHECK(p.empty());
  CHECK(p.capacity() == 8);
  CHECK_FALSE(p.isAlive(EntityId::invalid()));
  CHECK_FALSE(p.isAlive(EntityId{0, 0}));
}

TEST_CASE("Pool spawn takes the lowest free slot and value-initialises", "[core][pool]") {
  Pool<Thing, 4> p{};
  p.items[2].value = 99;  // garbage in a dead slot
  const EntityId a = p.spawn();
  const EntityId b = p.spawn();
  const EntityId c = p.spawn();
  CHECK(a == EntityId{0, 0});
  CHECK(b == EntityId{1, 0});
  CHECK(c == EntityId{2, 0});
  CHECK(p.get(c)->value == 0);
  CHECK(p.get(c)->other == 42);
  CHECK(p.size() == 3);
  p.despawn(b);
  CHECK(p.spawn() == EntityId{1, 1});
  CHECK(p.spawn() == EntityId{3, 0});
  CHECK(p.full());
  CHECK_FALSE(p.spawn().isValid());
  CHECK(p.size() == 4);
}

TEST_CASE("Pool stale handles are rejected", "[core][pool]") {
  Pool<Thing, 4> p{};
  const EntityId a = p.spawn();
  p.get(a)->value = 7;
  p.despawn(a);
  CHECK_FALSE(p.isAlive(a));
  CHECK(p.get(a) == nullptr);
  const EntityId a2 = p.spawn();
  CHECK(a2.index == a.index);
  CHECK(a2.generation == a.generation + 1);
  CHECK(p.get(a) == nullptr);
  CHECK(p.get(a2) != nullptr);
  CHECK(p.get(a2)->value == 0);
  p.despawn(a);  // stale: no-op
  CHECK(p.isAlive(a2));
  CHECK(p.size() == 1);
}

TEST_CASE("Pool spawnAt", "[core][pool]") {
  Pool<Thing, 4> p{};
  CHECK(p.spawnAt(2) == EntityId{2, 0});
  CHECK_FALSE(p.spawnAt(2).isValid());
  CHECK_FALSE(p.spawnAt(4).isValid());
  CHECK(p.spawn() == EntityId{0, 0});
  CHECK(p.idAt(2) == EntityId{2, 0});
}

TEST_CASE("Pool iterates alive slots in index order", "[core][pool]") {
  Pool<Thing, 130> p{};  // spans three 64-bit words
  std::vector<uint16_t> want;
  for (uint16_t i = 0; i < 130; i += 7) {
    REQUIRE(p.spawnAt(i).isValid());
    p.items[i].value = i;
    want.push_back(i);
  }
  std::vector<uint16_t> got;
  p.forEachAlive([&](EntityId id, Thing& t) {
    CHECK(t.value == id.index);
    got.push_back(id.index);
  });
  CHECK(got == want);

  const Pool<Thing, 130>& cp = p;
  size_t count = 0;
  cp.forEachAlive([&](EntityId, const Thing&) { ++count; });
  CHECK(count == want.size());
}

TEST_CASE("Pool despawn during iteration is safe", "[core][pool]") {
  Pool<Thing, 8> p{};
  for (int i = 0; i < 8; ++i)
    p.spawn();
  p.forEachAlive([&](EntityId id, Thing&) {
    if (id.index % 2 == 0) p.despawn(id);
  });
  CHECK(p.size() == 4);
  p.forEachAlive([](EntityId id, Thing&) { CHECK(id.index % 2 == 1); });
}

TEST_CASE("Pool clear bumps generations", "[core][pool]") {
  Pool<Thing, 4> p{};
  const EntityId a = p.spawn();
  p.clear();
  CHECK(p.empty());
  CHECK_FALSE(p.isAlive(a));
  CHECK(p.spawn() == EntityId{0, 1});
}

TEST_CASE("Pool survives memcpy snapshot and restore", "[core][pool]") {
  Pool<Thing, 4> p{};
  const EntityId a = p.spawn();
  p.get(a)->value = 5;
  Pool<Thing, 4> snap{};
  std::memcpy(&snap, &p, sizeof(p));
  p.despawn(a);
  std::memcpy(&p, &snap, sizeof(p));
  REQUIRE(p.isAlive(a));
  CHECK(p.get(a)->value == 5);
}
