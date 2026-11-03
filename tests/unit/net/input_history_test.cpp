#include "kfm/net/input_history.hpp"

#include "kfm/net/loopback.hpp"
#include "kfm/net/messages.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

sim::PlayerInput input(uint32_t k) {
  return sim::PlayerInput{static_cast<uint8_t>(k), static_cast<uint8_t>(k + 1),
                          static_cast<uint8_t>(k + 2), static_cast<uint8_t>(k & 3),
                          static_cast<uint8_t>(k + 4)};
}

// REQUIREs a value and returns it (value_or keeps clang-tidy's optional
// access check satisfied after the REQUIRE).
template <class T>
T unwrap(const std::optional<T>& o) {
  REQUIRE(o.has_value());
  return o.value_or(T{});
}

constexpr PlayerId kP0{0};
constexpr PlayerId kP1{1};

}  // namespace

TEST_CASE("SlotInputHistory stores by tick and repeats the last input", "[net][history]") {
  SlotInputHistory h;
  CHECK_FALSE(h.newest().has_value());
  CHECK(h.getOrRepeat(Tick{5}) == sim::PlayerInput{});

  CHECK(h.put(Tick{10}, input(10)));
  CHECK(h.put(Tick{12}, input(12)));
  CHECK(h.newest() == Tick{12});
  CHECK(h.has(Tick{10}));
  CHECK_FALSE(h.has(Tick{11}));
  CHECK(h.get(Tick{12}) == input(12));
  CHECK_FALSE(h.get(Tick{11}).has_value());

  bool exact = true;
  CHECK(h.getOrRepeat(Tick{11}, &exact) == input(10));
  CHECK_FALSE(exact);
  CHECK(h.getOrRepeat(Tick{12}, &exact) == input(12));
  CHECK(exact);
  CHECK(h.getOrRepeat(Tick{40}) == input(12));
  CHECK(h.getOrRepeat(Tick{9}) == sim::PlayerInput{});  // nothing before it

  // The window: kInputHistoryTicks ticks back from the newest.
  const Tick far{12 + kInputHistoryTicks};
  CHECK(h.put(far, input(1)));
  CHECK_FALSE(h.has(Tick{12}));            // evicted by the same index
  CHECK_FALSE(h.put(Tick{12}, input(2)));  // too old now
  CHECK(h.put(Tick{13}, input(13)));       // still inside
  CHECK(h.has(Tick{13}));

  h.clear();
  CHECK_FALSE(h.newest().has_value());
  CHECK_FALSE(h.has(Tick{13}));
}

TEST_CASE("InputHistory builds the redundant InputMsg window", "[net][history]") {
  InputHistory h;
  for (uint32_t t = 0; t < 40; ++t) {
    CHECK(h.put(kP0, Tick{t}, input(t)));
    if (t % 2 == 0) CHECK(h.put(PlayerId{3}, Tick{t}, input(100 + t)));
  }
  CHECK_FALSE(h.put(PlayerId{sim::kMaxPlayers}, Tick{1}, input(1)));
  CHECK_FALSE(h.put(PlayerId::invalid(), Tick{1}, input(1)));

  const InputMsg m = h.makeInputMsg(0b1001, Tick{39}, 16, Tick{30});
  CHECK(m.startTick == Tick{24});
  CHECK(m.count == 16);
  CHECK(m.slots == 0b1001);
  CHECK(m.lastAuthTick == Tick{30});
  for (uint32_t i = 0; i < 16; ++i) {
    CHECK(m.inputs[0][i] == input(24 + i));
    const uint32_t t = 24 + i;
    CHECK(m.inputs[3][i] == input(100 + (t % 2 == 0 ? t : t - 1)));  // odd ticks repeat
  }
  std::vector<std::byte> buf(kMaxUnreliableBytes);
  CHECK(encode(m, buf) > 0);

  // Near tick 0 the window shrinks rather than reaching before the match.
  const InputMsg early = h.makeInputMsg(0b1, Tick{2}, 16, std::nullopt);
  CHECK(early.startTick == Tick{0});
  CHECK(early.count == 3);
  CHECK(h.makeInputMsg(0b1, Tick{20}, 0, std::nullopt).count == 1);
  CHECK(h.makeInputMsg(0b1, Tick{20}, 200, std::nullopt).count == kMaxInputRedundancy);
}

TEST_CASE("FrameHistory builds AuthInputs from contiguous frames", "[net][history]") {
  FrameHistory h;
  CHECK_FALSE(h.makeAuthInputsMsg(Tick{0}, 16).has_value());
  for (uint32_t t = 0; t < 30; ++t) {
    if (t == 20) continue;  // a gap
    sim::InputFrame f;
    f.tick = Tick{t};
    f.present = 0b11;
    f.inputs[1] = input(t);
    CHECK(h.put(f));
  }
  CHECK(h.newest() == Tick{29});
  REQUIRE(h.get(Tick{5}) != nullptr);
  CHECK(h.get(Tick{5})->inputs[1] == input(5));
  CHECK(h.get(Tick{20}) == nullptr);

  const AuthInputsMsg m = unwrap(h.makeAuthInputsMsg(Tick{29}, 16));
  CHECK(m.startTick == Tick{21});  // stops at the gap
  CHECK(m.count == 9);
  for (uint32_t i = 0; i < m.count; ++i)
    CHECK(m.frames[i].tick == Tick{21 + i});
  const AuthInputsMsg early = unwrap(h.makeAuthInputsMsg(Tick{3}, 16));
  CHECK(early.startTick == Tick{0});
  CHECK(early.count == 4);
  CHECK_FALSE(h.makeAuthInputsMsg(Tick{20}, 4).has_value());

  std::vector<std::byte> buf(kMaxUnreliableBytes);
  const size_t n = encode(m, buf);
  REQUIRE(n > 0);
  AuthInputsMsg back;
  REQUIRE(decode({buf.data(), n}, back).has_value());
  CHECK(back == m);
}

TEST_CASE("InputFrameAssembler substitutes and finalises", "[net][history]") {
  InputFrameAssembler a(Tick{100});
  a.setPresent(0b11);
  CHECK(a.nextTick() == Tick{100});
  CHECK(a.put(kP0, Tick{100}, input(1)) == InputFrameAssembler::PutResult::Accepted);
  CHECK(a.put(kP0, Tick{101}, input(2)) == InputFrameAssembler::PutResult::Accepted);
  CHECK(a.put(kP0, Tick{99}, input(9)) == InputFrameAssembler::PutResult::Late);
  CHECK(a.put(kP0, Tick{100 + InputFrameAssembler::kMaxLeadTicks}, input(9)) ==
        InputFrameAssembler::PutResult::TooEarly);
  CHECK(a.put(PlayerId{sim::kMaxPlayers}, Tick{100}, input(9)) ==
        InputFrameAssembler::PutResult::BadSlot);

  sim::PlayerMask missing = 0xFFFF;
  const sim::InputFrame f100 = a.assembleNext(&missing);
  CHECK(f100.tick == Tick{100});
  CHECK(f100.present == 0b11);
  CHECK(f100.inputs[0] == input(1));
  CHECK(f100.inputs[1] == sim::PlayerInput{});  // nothing yet: idle
  CHECK(missing == 0b10);

  CHECK(a.put(kP1, Tick{101}, input(7)) == InputFrameAssembler::PutResult::Accepted);
  const sim::InputFrame f101 = a.assembleNext(&missing);
  CHECK(missing == 0);
  CHECK(f101.inputs[0] == input(2));
  CHECK(f101.inputs[1] == input(7));

  // Slot 0 goes quiet: its last input repeats, and a late arrival is refused
  // so the authoritative stream never changes after the fact.
  const sim::InputFrame f102 = a.assembleNext(&missing);
  CHECK(missing == 0b11);
  CHECK(f102.inputs[0] == input(2));
  CHECK(f102.inputs[1] == input(7));
  CHECK(a.put(kP0, Tick{102}, input(5)) == InputFrameAssembler::PutResult::Late);
  CHECK(a.history().getOrRepeat(kP0, Tick{102}) == input(2));

  // A slot that is not present gets no input; a slot that left starts idle.
  a.setPresent(0b01);
  const sim::InputFrame f103 = a.assembleNext();
  CHECK(f103.present == 0b01);
  CHECK(f103.inputs[1] == sim::PlayerInput{});
  a.clearSlot(kP1);
  a.setPresent(0b11);
  CHECK(a.assembleNext().inputs[1] == sim::PlayerInput{});

  a.reset(Tick{0});
  CHECK(a.nextTick() == Tick{0});
  CHECK(a.assembleNext().inputs[0] == sim::PlayerInput{});
}

// Inputs flow client history → InputMsg → lossy loopback → assembler →
// FrameHistory → AuthInputs → client, with 16-tick redundancy hiding 20 %
// loss (§6.3: a loss costs nothing unless many consecutive packets vanish).
TEST_CASE("Redundant inputs survive a lossy link end to end", "[net][history][loopback]") {
  constexpr uint64_t kTickNs = 1'000'000'000ull / sim::kTickHz;
  LoopbackNetwork net(77);
  const LinkConditions link = LinkConditions::uniform(40'000'000, 10'000'000, 200'000);
  LoopbackTransport& client = net.connectClient(0, link, link);
  LoopbackTransport& server = net.server();
  PeerId clientId = PeerId::invalid();

  InputHistory local;
  InputFrameAssembler assembler(Tick{0});
  assembler.setPresent(0b1);
  FrameHistory serverFrames;
  FrameHistory clientFrames;
  std::vector<std::byte> buf(kMaxUnreliableBytes);
  // Ticks the client runs ahead: the one-way delay is up to 3 ticks, so about
  // 7 redundant copies of each input can arrive in time; losing all of them
  // at 20 % loss is a 1e-5 event.
  constexpr int32_t kLead = 10;
  uint32_t substitutedTicks = 0;

  for (uint32_t tick = 0; tick < 1200; ++tick) {
    const uint64_t now = tick * kTickNs;
    ConnectionEvents ev;
    client.poll(now, ev);
    server.poll(now, ev);
    for (const ConnectionEvent& e : ev.span()) {
      if (e.kind == ConnectionEventKind::Connected && e.peer != kServerPeer) clientId = e.peer;
    }
    Packet p;
    while (server.recv(p)) {
      InputMsg m;
      REQUIRE(decode(p.data, m).has_value());
      for (uint8_t i = 0; i < m.count; ++i)
        static_cast<void>(assembler.put(kP0, m.startTick.plus(i), m.inputs[0][i]));
    }
    while (client.recv(p)) {
      AuthInputsMsg m;
      REQUIRE(decode(p.data, m).has_value());
      for (uint8_t i = 0; i < m.count; ++i)
        static_cast<void>(clientFrames.put(m.frames[i]));
    }

    // Client: tick L = server tick + lead.
    if (client.isConnected(kServerPeer)) {
      const Tick l{tick + kLead};
      CHECK(local.put(kP0, l, sim::PlayerInput{static_cast<uint8_t>(tick / 20), 255, 0, 0, 0}));
      const InputMsg m = local.makeInputMsg(0b1, l, kMaxInputRedundancy, clientFrames.newest());
      const size_t n = encode(m, buf);
      REQUIRE(n > 0);
      client.sendUnreliable(kServerPeer, {buf.data(), n});
    }
    // Server: assemble this tick and broadcast the last 16 frames.
    sim::PlayerMask missing = 0;
    const sim::InputFrame f = assembler.assembleNext(&missing);
    if (tick > 20 && missing != 0) ++substitutedTicks;
    CHECK(serverFrames.put(f));
    if (clientId.isValid()) {
      const AuthInputsMsg auth =
          unwrap(serverFrames.makeAuthInputsMsg(f.tick, kMaxInputRedundancy));
      const size_t n = encode(auth, buf);
      REQUIRE(n > 0);
      server.sendUnreliable(clientId, {buf.data(), n});
    }
  }

  CHECK(substitutedTicks == 0);
  // The client holds the authoritative stream, identical to the server's.
  const Tick newest = unwrap(clientFrames.newest());
  uint32_t compared = 0;
  for (uint32_t t = 1100; t < newest.value; ++t) {
    const sim::InputFrame* c = clientFrames.get(Tick{t});
    const sim::InputFrame* s = serverFrames.get(Tick{t});
    REQUIRE(c != nullptr);
    REQUIRE(s != nullptr);
    CHECK(*c == *s);
    CHECK(c->inputs[0].moveDir == static_cast<uint8_t>((t - kLead) / 20));
    ++compared;
  }
  CHECK(compared > 50);
}
