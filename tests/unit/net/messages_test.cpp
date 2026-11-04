#include "kfm/net/messages.hpp"

#include "kfm/core/bitstream.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

template <class T>
T roundTrip(const T& m, size_t* bytes = nullptr) {
  std::vector<std::byte> buf(kMaxReliableBytes);
  const size_t n = encode(m, buf);
  REQUIRE(n > 0);
  if (bytes != nullptr) *bytes = n;
  const std::span<const std::byte> wire(buf.data(), n);
  REQUIRE(peekType(wire).has_value());
  T out{};
  const DecodeResult r = decode(wire, out);
  REQUIRE(r.has_value());
  CHECK(out == m);
  return out;
}

sim::PlayerInput varied(uint32_t k) {
  return sim::PlayerInput{static_cast<uint8_t>(k * 7), static_cast<uint8_t>(k * 13 + 1),
                          static_cast<uint8_t>(k * 29 + 2), static_cast<uint8_t>(k * 3 + 3),
                          static_cast<uint8_t>(k * 11 + 4)};
}

// Every field of every input differs from the one before: the worst case.
InputMsg worstInputMsg() {
  InputMsg m;
  m.startTick = Tick{0xFFFF'0000u};
  m.count = kMaxInputRedundancy;
  m.slots = kAllPlayersMask;
  m.lastAuthTick = Tick{0xFFFE'FFFFu};
  for (uint32_t s = 0; s < sim::kMaxPlayers; ++s) {
    for (uint32_t i = 0; i < kMaxInputRedundancy; ++i)
      m.inputs[s][i] = varied(s * 100 + i + 1);
  }
  return m;
}

AuthInputsMsg worstAuthInputsMsg() {
  AuthInputsMsg m;
  m.startTick = Tick{1000};
  m.count = kMaxInputRedundancy;
  m.arrivalMargin = -3;
  m.checksumTick = Tick{1016};
  m.checksum = 0xDEAD'BEEFu;
  for (uint32_t i = 0; i < kMaxInputRedundancy; ++i) {
    sim::InputFrame& f = m.frames[i];
    f.tick = m.startTick.plus(static_cast<int32_t>(i));
    f.present = kAllPlayersMask;
    for (uint32_t s = 0; s < sim::kMaxPlayers; ++s)
      f.inputs[s] = varied(s * 100 + i + 1);
  }
  return m;
}

}  // namespace

TEST_CASE("Input delta coding round-trips and is one bit when unchanged", "[net][messages]") {
  std::array<std::byte, 16> buf{};
  const sim::PlayerInput a = varied(1);
  sim::PlayerInput b = a;
  b.buttons = 0x13;
  for (const auto& [prev, in, bits] :
       std::vector<std::tuple<sim::PlayerInput, sim::PlayerInput, size_t>>{
           {a, a, 1}, {a, b, 1 + 5 + 8}, {sim::PlayerInput{}, a, kInputDeltaMaxBits}}) {
    BitWriter w(buf);
    writeInputDelta(w, prev, in);
    CHECK(w.bitsWritten() == bits);
    BitReader r(w.written());
    sim::PlayerInput out;
    readInputDelta(r, prev, out);
    CHECK(out == in);
  }
}

TEST_CASE("Hot-path messages round-trip", "[net][messages]") {
  InputMsg in;
  in.startTick = Tick{500};
  in.count = 8;
  in.slots = 0b0000000101;  // two local players
  for (uint32_t i = 0; i < 8; ++i) {
    in.inputs[0][i] = varied(i / 3);  // changes now and then
    in.inputs[2][i] = sim::PlayerInput{64, 255, 64, 0, 0};
  }
  roundTrip(in);
  in.lastAuthTick = Tick{490};
  roundTrip(in);
  in.count = 1;
  in.slots = 1u << 9;
  roundTrip(in);

  AuthInputsMsg a;
  a.startTick = Tick{0};
  a.count = 3;
  for (uint32_t i = 0; i < 3; ++i) {
    a.frames[i].tick = Tick{i};
    a.frames[i].present = 0b11;
    a.frames[i].inputs[1] = varied(i);
  }
  a.frames[2].present = 0b111;  // a player joins mid-window
  a.frames[2].inputs[2] = varied(9);
  roundTrip(a);
  a.arrivalMargin = 2;
  a.checksumTick = Tick{3};
  a.checksum = 77;
  roundTrip(a);
  a.checksumTick = Tick{0};
  roundTrip(a);
  a.count = 1;
  a.frames[0].present = 0;  // empty lobby
  roundTrip(a);

  size_t n = 0;
  roundTrip(PingMsg{0x0123'4567'89AB'CDEFull, Tick{42}}, &n);
  CHECK(n == kPingMsgBytes);
  roundTrip(PongMsg{~0ull, Tick{43}, 1'000'000}, &n);
  CHECK(n == kPongMsgBytes);
}

TEST_CASE("Unreliable messages fit one datagram at the worst case", "[net][messages]") {
  size_t inputBytes = 0;
  roundTrip(worstInputMsg(), &inputBytes);
  size_t authBytes = 0;
  roundTrip(worstAuthInputsMsg(), &authBytes);
  INFO("worst Input " << inputBytes << " B, worst AuthInputs " << authBytes << " B");
  CHECK(inputBytes <= kInputMsgMaxBytes);
  CHECK(authBytes <= kAuthInputsMsgMaxBytes);
  CHECK(inputBytes <= kMaxUnreliableBytes);
  CHECK(authBytes <= kMaxUnreliableBytes);
  // The bounds are exact: the worst case reaches them.
  CHECK(inputBytes == kInputMsgMaxBytes);
  CHECK(authBytes == kAuthInputsMsgMaxBytes);

  // The usual case: 10 players holding their sticks for 16 ticks.
  AuthInputsMsg steady;
  steady.startTick = Tick{100};
  steady.count = kMaxInputRedundancy;
  for (uint32_t i = 0; i < kMaxInputRedundancy; ++i) {
    steady.frames[i].tick = steady.startTick.plus(static_cast<int32_t>(i));
    steady.frames[i].present = kAllPlayersMask;
    for (uint32_t s = 0; s < sim::kMaxPlayers; ++s)
      steady.frames[i].inputs[s] = varied(s);
  }
  size_t steadyBytes = 0;
  roundTrip(steady, &steadyBytes);
  INFO("steady AuthInputs " << steadyBytes << " B");
  CHECK(steadyBytes < 100);
}

TEST_CASE("Control messages round-trip", "[net][messages]") {
  HelloMsg h;
  h.contentHash = 0xC0FFEE;
  h.buildId = 0xB01D;
  h.tokenLength = kMaxTokenBytes;
  for (uint8_t i = 0; i < kMaxTokenBytes; ++i)
    h.token[i] = static_cast<std::byte>(i * 5 + 1);
  h.requestedSlots = 2;
  roundTrip(h);
  h.tokenLength = 0;
  h.requestedSlots = 0;
  roundTrip(h);

  roundTrip(WelcomeMsg{0b1100, sim::MatchConfig{99, 0x3FF}, Tick{1234}});
  roundTrip(WelcomeMsg{0, sim::MatchConfig{1, 1}, Tick{0}});  // spectator
  roundTrip(WelcomeMsg{0b1, sim::MatchConfig{5, 0b11, 2, sim::kMaxModes - 1}, Tick{7}});
  // v4: the loadout rides along (MatchConfig::loadout, v0.31).
  roundTrip(WelcomeMsg{
      0b1, sim::MatchConfig{5, 0b11, 2, 3, sim::kAllPowerUps, sim::kMaxLoadouts - 1}, Tick{7}});
  for (uint8_t r = 0; r < kRejectReasonCount; ++r)
    roundTrip(RejectMsg{static_cast<RejectReason>(r)});
  roundTrip(JoinMatchMsg{0});
  roundTrip(JoinMatchMsg{sim::kMaxPlayers});
  roundTrip(PlayerJoinedMsg{PlayerId{9}, Tick{77}, true});
  roundTrip(PlayerJoinedMsg{PlayerId{0}, Tick{0}, false});
  roundTrip(PlayerLeftMsg{PlayerId{3}, Tick{5000}});
  roundTrip(MatchConfigMsg{sim::MatchConfig{0xFFFF'FFFF'FFFF'FFFFull, 0x2AA}, Tick{60}});
  roundTrip(MatchConfigMsg{sim::MatchConfig{3, 0x1, sim::kMaxArenas - 1, 4}, Tick{61}});
  // The per-match power-up set (protocol v3): none, some, all.
  roundTrip(MatchConfigMsg{sim::MatchConfig{3, 0x1, 1, 1, 0}, Tick{62}});
  roundTrip(WelcomeMsg{0b1, sim::MatchConfig{5, 0b11, 0, 0, 0x0021}, Tick{8}});
  roundTrip(MatchConfigMsg{sim::MatchConfig{3, 0x1, 1, 1, sim::kAllPowerUps}, Tick{63}});
  for (uint8_t r = 0; r < kDisconnectReasonCount; ++r)
    roundTrip(DisconnectMsg{static_cast<DisconnectReason>(r)});
  roundTrip(ResyncRequestMsg{Tick{0}, 0});
  roundTrip(ResyncRequestMsg{Tick{0xFFFF'FFFFu}, 0xDEAD'BEEFu});
}

TEST_CASE("Encoders refuse invalid messages", "[net][messages]") {
  std::vector<std::byte> buf(kMaxReliableBytes);
  InputMsg in;
  in.slots = 1;
  in.count = 0;
  CHECK(encode(in, buf) == 0);
  in.count = kMaxInputRedundancy + 1;
  CHECK(encode(in, buf) == 0);
  in.count = 4;
  in.slots = 0;
  CHECK(encode(in, buf) == 0);
  in.slots = 1u << sim::kMaxPlayers;
  CHECK(encode(in, buf) == 0);
  in.slots = 1;
  in.startTick = Tick{0xFFFF'FFFEu};  // the window would wrap
  CHECK(encode(in, buf) == 0);

  AuthInputsMsg a;
  a.startTick = Tick{1000};
  a.count = 2;
  a.checksumTick = Tick{1003};  // after the window's end
  CHECK(encode(a, buf) == 0);
  a.checksumTick = Tick{1002 - 256};  // too far back
  CHECK(encode(a, buf) == 0);
  a.checksumTick.reset();
  a.frames[1].present = 0x8000;
  CHECK(encode(a, buf) == 0);

  HelloMsg h;
  h.tokenLength = kMaxTokenBytes + 1;
  CHECK(encode(h, buf) == 0);
  h.tokenLength = 0;
  h.requestedSlots = sim::kMaxPlayers + 1;
  CHECK(encode(h, buf) == 0);
  CHECK(encode(RejectMsg{static_cast<RejectReason>(kRejectReasonCount)}, buf) == 0);
  CHECK(encode(DisconnectMsg{static_cast<DisconnectReason>(kDisconnectReasonCount)}, buf) == 0);
  CHECK(encode(PlayerJoinedMsg{PlayerId{sim::kMaxPlayers}, Tick{}, false}, buf) == 0);
  CHECK(encode(PlayerLeftMsg{PlayerId::invalid(), Tick{}}, buf) == 0);
  CHECK(encode(JoinMatchMsg{sim::kMaxPlayers + 1}, buf) == 0);
  CHECK(encode(WelcomeMsg{0x400, {}, {}}, buf) == 0);
  CHECK(encode(MatchConfigMsg{sim::MatchConfig{0, 0xFFFF}, {}}, buf) == 0);
  // Arena and mode indices beyond the content capacities never go out.
  CHECK(encode(MatchConfigMsg{sim::MatchConfig{0, 1, sim::kMaxArenas, 0}, {}}, buf) == 0);
  CHECK(encode(WelcomeMsg{1, sim::MatchConfig{0, 1, 0, sim::kMaxModes}, {}}, buf) == 0);
  CHECK(
      encode(WelcomeMsg{1, sim::MatchConfig{0, 1, 0, 0, sim::kAllPowerUps, sim::kMaxLoadouts}, {}},
             buf) == 0);

  // Too small a buffer is an overflow, not a partial message.
  std::array<std::byte, 8> small{};
  CHECK(encode(worstInputMsg(), small) == 0);
  CHECK(encode(PingMsg{}, small) == 0);
}

TEST_CASE("Decoders reject malformed bytes cleanly", "[net][messages]") {
  std::vector<std::byte> buf(kMaxReliableBytes);
  const size_t n = encode(PongMsg{1, Tick{2}, 3}, buf);
  REQUIRE(n == kPongMsgBytes);
  PongMsg pong;
  PingMsg ping;
  CHECK(decode({buf.data(), n}, ping).error() == DecodeError::WrongType);
  CHECK(decode({buf.data(), n - 1}, pong).error() == DecodeError::Truncated);
  CHECK(decode({buf.data(), 0}, pong).error() == DecodeError::Truncated);
  buf[n] = std::byte{0};
  CHECK(decode({buf.data(), n + 1}, pong).error() == DecodeError::TrailingBytes);

  CHECK(peekType({}).error() == DecodeError::Truncated);
  const std::array<std::byte, 1> zero{std::byte{0}};
  CHECK(peekType(zero).error() == DecodeError::WrongType);
  const std::array<std::byte, 1> high{std::byte{kMessageTypeCount + 1}};
  CHECK(peekType(high).error() == DecodeError::WrongType);

  // Out-of-range fields.
  const std::array<std::byte, 2> reject{std::byte{static_cast<uint8_t>(MessageType::Reject)},
                                        std::byte{200}};
  RejectMsg rj;
  CHECK(decode(reject, rj).error() == DecodeError::OutOfRange);
  const std::array<std::byte, 2> join{std::byte{static_cast<uint8_t>(MessageType::JoinMatch)},
                                      std::byte{sim::kMaxPlayers + 1}};
  JoinMatchMsg jm;
  CHECK(decode(join, jm).error() == DecodeError::OutOfRange);

  // Non-zero padding bits after a message are trailing garbage.
  InputMsg in;
  in.slots = 1;
  in.count = 2;  // 57 bits: seven padding bits
  const size_t m = encode(in, buf);
  REQUIRE(m > 0);
  InputMsg decoded;
  REQUIRE(decode({buf.data(), m}, decoded).has_value());
  buf[m - 1] |= std::byte{0x80};
  CHECK(decode({buf.data(), m}, decoded).error() == DecodeError::TrailingBytes);

  // A Hello of another protocol version still yields its version.
  HelloMsg h;
  h.protocolVersion = kProtocolVersion + 7;
  const size_t hn = encode(h, buf);
  REQUIRE(hn > 0);
  CHECK(peekHelloVersion({buf.data(), hn}).value() == kProtocolVersion + 7);
  CHECK(peekHelloVersion({buf.data(), 2}).error() == DecodeError::Truncated);
  CHECK(peekHelloVersion({buf.data() + 0, 0}).error() == DecodeError::Truncated);
}

namespace {

// Advances a scripted 10-player match by `ticks`, keeping the largest
// snapshot seen.
size_t largestSnapshotOfMatch(uint32_t ticks, sim::WorldState& world) {
  const sim::ContentDB& content = sim::defaultContent();
  const sim::MatchConfig cfg{1234, kAllPlayersMask};
  world = sim::initialWorld(cfg);
  Rng inputRng = Rng::seeded(99);
  sim::EventSink sink;
  std::vector<std::byte> buf(kMaxReliableBytes);
  auto snap = std::make_unique<SnapshotMsg>();
  snap->config = cfg;
  size_t largest = 0;
  for (uint32_t t = 0; t < ticks; ++t) {
    const sim::InputFrame f = sim::scriptedFrame(inputRng, world, content);
    sink.clear();
    sim::step(world, f, content, sink);
    if (t % 30 == 0) {
      snap->world = world;
      largest = std::max(largest, encode(*snap, buf));
    }
  }
  return largest;
}

}  // namespace

TEST_CASE("Snapshot round-trips and stays far below the reliable limit", "[net][messages]") {
  auto world = std::make_unique<sim::WorldState>();
  const size_t matchMax = largestSnapshotOfMatch(1800, *world);
  REQUIRE(matchMax > 0);

  auto snap = std::make_unique<SnapshotMsg>();
  snap->config = sim::MatchConfig{1234, kAllPlayersMask, 1, 2};
  snap->world = *world;
  std::vector<std::byte> buf(kMaxReliableBytes);
  const size_t n = encode(*snap, buf);
  REQUIRE(n > 0);
  auto back = std::make_unique<SnapshotMsg>();
  REQUIRE(decode({buf.data(), n}, *back).has_value());
  CHECK(sim::checksum(back->world) == sim::checksum(snap->world));
  CHECK(back->config.seed == 1234);
  CHECK(back->config.players == kAllPlayersMask);
  CHECK(back->config.arena == 1);
  CHECK(back->config.mode == 2);

  // Worst case: every pool full.
  auto full = std::make_unique<SnapshotMsg>();
  full->world = sim::initialWorld(sim::MatchConfig{1, kAllPlayersMask});
  sim::WorldState& w = full->world;
  while (!w.players.full()) {
    const EntityId id = w.players.spawnAt(w.players.size());
    // Bounds the index for GCC 15's -Wstringop-overflow in Release.
    if (id.index >= sim::kMaxPlayers) FAIL("spawnAt failed");
    w.players.items[id.index].id = PlayerId{static_cast<PlayerId::rep>(id.index)};
    w.players.items[id.index].modifiers.count = sim::kMaxModifiers;
  }
  while (!w.boomerangs.full())
    static_cast<void>(w.boomerangs.spawn());
  while (!w.pickups.full())
    static_cast<void>(w.pickups.spawn());
  const size_t worst = encode(*full, buf);
  REQUIRE(worst > 0);
  REQUIRE(decode({buf.data(), worst}, *back).has_value());
  CHECK(sim::checksum(back->world) == sim::checksum(full->world));
  INFO("snapshot: scripted match max " << matchMax << " B, all pools full " << worst << " B");
  CHECK(worst < kMaxReliableBytes / 4);  // no compression needed for M3
  CHECK(matchMax <= worst);

  // A flipped bit in the world fails the checksum (or the visitor).
  buf[worst / 2] ^= std::byte{0x10};
  const DecodeResult bad = decode({buf.data(), worst}, *back);
  REQUIRE_FALSE(bad.has_value());
  CHECK((bad.error() == DecodeError::ChecksumMismatch || bad.error() == DecodeError::InvalidState));
  CHECK(decode({buf.data(), worst - 1}, *back).error() == DecodeError::Truncated);
}
