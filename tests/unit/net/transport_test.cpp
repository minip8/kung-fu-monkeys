#include "kfm/net/transport.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace kfm::net;

TEST_CASE("PeerId validity and ordering", "[net][transport]") {
  CHECK_FALSE(PeerId::invalid().isValid());
  CHECK(kServerPeer.isValid());
  CHECK(PeerId{1} > kServerPeer);
}

TEST_CASE("ConnectionEvents is bounded", "[net][transport]") {
  ConnectionEvents ev;
  for (size_t i = 0; i < ConnectionEvents::kCapacity + 3; ++i)
    ev.push(ConnectionEvent{ConnectionEventKind::Connected, PeerId{1}, i});
  CHECK(ev.count == ConnectionEvents::kCapacity);
  CHECK(ev.dropped == 3);
  CHECK(ev.span().size() == ConnectionEvents::kCapacity);
  ev.clear();
  CHECK(ev.count == 0);
  CHECK(ev.dropped == 0);
}
