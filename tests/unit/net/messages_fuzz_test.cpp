// Property test for every decoder (architecture §6.7: message decoders are
// fuzzed). Random and mutated byte strings must never crash a decoder, and
// whatever a decoder accepts must encode again and decode to the same value.
// libFuzzer targets can wrap the same check later (§9).
#include "kfm/core/rng.hpp"
#include "kfm/net/messages.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace kfm;
using namespace kfm::net;

namespace {

template <class T>
[[nodiscard]] bool sameValue(const T& a, const T& b) {
  return a == b;
}
template <>
[[nodiscard]] bool sameValue(const SnapshotMsg& a, const SnapshotMsg& b) {
  return a.config.seed == b.config.seed && a.config.players == b.config.players &&
         sim::checksum(a.world) == sim::checksum(b.world);
}

struct Tally {
  uint64_t accepted{0};
  uint64_t rejected{0};
};

// Decodes `bytes`; on success re-encodes and decodes again.
template <class T>
void checkOne(std::span<const std::byte> bytes, T& scratch, T& again, std::vector<std::byte>& buf,
              Tally& tally) {
  const DecodeResult r = decode(bytes, scratch);
  if (!r) {
    ++tally.rejected;
    return;
  }
  ++tally.accepted;
  const size_t n = encode(scratch, buf);
  REQUIRE(n > 0);
  REQUIRE(decode({buf.data(), n}, again).has_value());
  REQUIRE(sameValue(scratch, again));
  // The type survives peeking too.
  REQUIRE(peekType({buf.data(), n}).has_value());
}

void mutate(Rng& rng, std::vector<std::byte>& v) {
  switch (rng.below(5)) {
    case 0:  // flip a bit
      if (!v.empty()) v[rng.below(static_cast<uint32_t>(v.size()))] ^= std::byte{1} << rng.below(8);
      break;
    case 1:  // overwrite a byte
      if (!v.empty())
        v[rng.below(static_cast<uint32_t>(v.size()))] = static_cast<std::byte>(rng.below(256));
      break;
    case 2:  // truncate
      v.resize(rng.below(static_cast<uint32_t>(v.size()) + 1));
      break;
    case 3:  // extend
      v.push_back(static_cast<std::byte>(rng.below(256)));
      break;
    default:  // keep the type byte, randomise one region
      for (uint32_t i = 0; i < 4 && v.size() > 1; ++i)
        v[1 + rng.below(static_cast<uint32_t>(v.size() - 1))] =
            static_cast<std::byte>(rng.below(256));
      break;
  }
}

// Random strings that start with T's type byte (or not), plus mutations of
// valid encodings of the seeds.
template <class T>
Tally fuzz(MessageType type, const std::vector<T>& seeds, uint32_t iterations, uint64_t seed) {
  Rng rng = Rng::seeded(seed);
  std::vector<std::byte> buf(kMaxReliableBytes);
  auto scratch = std::make_unique<T>();
  auto again = std::make_unique<T>();
  Tally tally;

  std::vector<std::vector<std::byte>> valid;
  for (const T& s : seeds) {
    const size_t n = encode(s, buf);
    REQUIRE(n > 0);
    valid.emplace_back(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n));
  }

  std::vector<std::byte> bytes;
  for (uint32_t it = 0; it < iterations; ++it) {
    if (it % 4 == 0 || valid.empty()) {
      bytes.resize(rng.below(64));
      for (std::byte& b : bytes)
        b = static_cast<std::byte>(rng.below(256));
      if (!bytes.empty() && rng.below(4) != 0) bytes[0] = static_cast<std::byte>(type);
    } else {
      bytes = valid[rng.below(static_cast<uint32_t>(valid.size()))];
      const uint32_t rounds = 1 + rng.below(3);
      for (uint32_t m = 0; m < rounds; ++m)
        mutate(rng, bytes);
    }
    checkOne(bytes, *scratch, *again, buf, tally);
  }
  // The seeds themselves decode.
  for (const auto& v : valid) {
    REQUIRE(decode(v, *scratch).has_value());
  }
  return tally;
}

sim::PlayerInput input(uint32_t k) {
  return sim::PlayerInput{static_cast<uint8_t>(k), static_cast<uint8_t>(k * 3),
                          static_cast<uint8_t>(k * 5), static_cast<uint8_t>(k & 0x1F),
                          static_cast<uint8_t>(k * 7)};
}

}  // namespace

TEST_CASE("Hot-path decoders survive arbitrary bytes", "[net][messages][fuzz]") {
  std::vector<InputMsg> inputs(3);
  inputs[0].slots = 1;
  inputs[0].count = 1;
  inputs[1].startTick = Tick{1000};
  inputs[1].count = 16;
  inputs[1].slots = 0b1001;
  inputs[1].lastAuthTick = Tick{990};
  for (uint32_t i = 0; i < 16; ++i) {
    inputs[1].inputs[0][i] = input(i / 4);
    inputs[1].inputs[3][i] = input(i);
  }
  inputs[2] = inputs[1];
  inputs[2].slots = kAllPlayersMask;
  const Tally in = fuzz(MessageType::Input, inputs, 20'000, 1);
  CHECK(in.accepted > 0);
  CHECK(in.rejected > 0);

  std::vector<AuthInputsMsg> auths(2);
  auths[0].frames[0].present = 0b11;
  auths[1].startTick = Tick{300};
  auths[1].count = 12;
  auths[1].arrivalMargin = 2;
  auths[1].checksumTick = Tick{310};
  auths[1].checksum = 0x1234'5678u;
  for (uint32_t i = 0; i < 12; ++i) {
    auths[1].frames[i].tick = Tick{300 + i};
    auths[1].frames[i].present = i < 6 ? 0b111 : 0b1111;
    for (uint32_t s = 0; s < 4; ++s)
      auths[1].frames[i].inputs[s] = input(s * 16 + i / 3);
  }
  const Tally au = fuzz(MessageType::AuthInputs, auths, 20'000, 2);
  CHECK(au.accepted > 0);
  CHECK(au.rejected > 0);

  const Tally pi = fuzz(MessageType::Ping, std::vector<PingMsg>{{5, Tick{6}}}, 5'000, 3);
  const Tally po = fuzz(MessageType::Pong, std::vector<PongMsg>{{5, Tick{6}, 7}}, 5'000, 4);
  CHECK(pi.accepted > 0);
  CHECK(po.accepted > 0);
}

TEST_CASE("Control decoders survive arbitrary bytes", "[net][messages][fuzz]") {
  HelloMsg h;
  h.contentHash = 1;
  h.buildId = 2;
  h.tokenLength = 16;
  h.requestedSlots = 2;
  CHECK(fuzz(MessageType::Hello, std::vector<HelloMsg>{h, HelloMsg{}}, 10'000, 5).accepted > 0);
  CHECK(fuzz(MessageType::Welcome,
             std::vector<WelcomeMsg>{{0b11, sim::MatchConfig{7, 0x3FF}, Tick{9}}}, 5'000, 6)
            .accepted > 0);
  CHECK(fuzz(MessageType::Reject, std::vector<RejectMsg>{{RejectReason::MatchFull}}, 5'000, 7)
            .accepted > 0);
  CHECK(fuzz(MessageType::JoinMatch, std::vector<JoinMatchMsg>{{1}}, 5'000, 8).accepted > 0);
  CHECK(fuzz(MessageType::PlayerJoined, std::vector<PlayerJoinedMsg>{{PlayerId{3}, Tick{4}, true}},
             5'000, 9)
            .accepted > 0);
  CHECK(fuzz(MessageType::PlayerLeft, std::vector<PlayerLeftMsg>{{PlayerId{3}, Tick{4}}}, 5'000, 10)
            .accepted > 0);
  CHECK(fuzz(MessageType::MatchConfig,
             std::vector<MatchConfigMsg>{{sim::MatchConfig{1, 3}, Tick{60}}}, 5'000, 11)
            .accepted > 0);
  CHECK(fuzz(MessageType::Disconnect, std::vector<DisconnectMsg>{{DisconnectReason::Timeout}},
             5'000, 12)
            .accepted > 0);
  CHECK(fuzz(MessageType::ResyncRequest, std::vector<ResyncRequestMsg>{{Tick{1234}, 0xC0FFEEu}},
             5'000, 14)
            .accepted > 0);
}

TEST_CASE("Room decoders survive arbitrary bytes", "[net][messages][fuzz][room]") {
  HelloMsg h;
  h.contentHash = 1;
  h.requestedSlots = 1;
  h.nickname = Nickname::from("\xF0\x9F\x90\x92 Mandrill");
  CHECK(fuzz(MessageType::Hello, std::vector<HelloMsg>{h}, 10'000, 15).accepted > 0);
  CHECK(fuzz(MessageType::RoomSettings,
             std::vector<RoomSettingsMsg>{{}, {RoomSettings{3, 1, 2, 0x00FF, 4}}}, 5'000, 16)
            .accepted > 0);
  CHECK(fuzz(MessageType::Start, std::vector<StartMsg>{{}}, 2'000, 17).accepted > 0);
  RoomStateMsg r;
  r.phase = RoomPhase::Playing;
  r.host = PlayerId{2};
  r.settings = RoomSettings{1, 2, 0, 0xFFFF, 3};
  r.humans = 0b1100;
  r.bots = 0b0011;
  r.botPlacement = 0b0011;
  r.winner = PlayerId{3};
  r.names[2] = Nickname::from("Gibbon");
  r.names[3] = Nickname::from("\xC3\xA9t\xC3\xA9");
  RoomStateMsg waiting;  // v7: a placement previewed while Waiting
  waiting.host = PlayerId{1};
  waiting.humans = 0b10;
  waiting.settings = RoomSettings{0, 0, 0, 0xFFFF, 2};
  waiting.botPlacement = 0b101;
  waiting.names[1] = Nickname::from("Mandrill");
  const Tally t = fuzz(MessageType::RoomState,
                       std::vector<RoomStateMsg>{r, waiting, RoomStateMsg{}}, 20'000, 18);
  CHECK(t.accepted > 0);
  CHECK(t.rejected > 0);
}

TEST_CASE("Snapshot decoder survives arbitrary bytes", "[net][messages][fuzz]") {
  std::vector<SnapshotMsg> seeds(1);
  seeds[0].config = sim::MatchConfig{3, 0b111};
  seeds[0].world = sim::initialWorld(seeds[0].config);
  const Tally t = fuzz(MessageType::Snapshot, seeds, 2'000, 13);
  CHECK(t.rejected > 0);
}

TEST_CASE("Every decoder rejects every other message type", "[net][messages][fuzz]") {
  std::vector<std::byte> buf(64);
  const size_t n = encode(PingMsg{1, Tick{2}}, buf);
  REQUIRE(n > 0);
  const std::span<const std::byte> ping(buf.data(), n);
  InputMsg a;
  AuthInputsMsg b;
  PongMsg c;
  HelloMsg d;
  WelcomeMsg e;
  RejectMsg f;
  JoinMatchMsg g;
  PlayerJoinedMsg h;
  PlayerLeftMsg i;
  MatchConfigMsg j;
  DisconnectMsg k;
  ResyncRequestMsg l;
  RoomSettingsMsg m;
  RoomStateMsg o;
  StartMsg q;
  auto snap = std::make_unique<SnapshotMsg>();
  CHECK(decode(ping, a).error() == DecodeError::WrongType);
  CHECK(decode(ping, b).error() == DecodeError::WrongType);
  CHECK(decode(ping, c).error() == DecodeError::WrongType);
  CHECK(decode(ping, d).error() == DecodeError::WrongType);
  CHECK(decode(ping, e).error() == DecodeError::WrongType);
  CHECK(decode(ping, f).error() == DecodeError::WrongType);
  CHECK(decode(ping, g).error() == DecodeError::WrongType);
  CHECK(decode(ping, h).error() == DecodeError::WrongType);
  CHECK(decode(ping, i).error() == DecodeError::WrongType);
  CHECK(decode(ping, j).error() == DecodeError::WrongType);
  CHECK(decode(ping, k).error() == DecodeError::WrongType);
  CHECK(decode(ping, l).error() == DecodeError::WrongType);
  CHECK(decode(ping, m).error() == DecodeError::WrongType);
  CHECK(decode(ping, o).error() == DecodeError::WrongType);
  CHECK(decode(ping, q).error() == DecodeError::WrongType);
  CHECK(decode(ping, *snap).error() == DecodeError::WrongType);
}
