// Wire layouts (bit counts; every message starts with the u8 type and ends
// with zero padding to a byte):
//
//   Input       startTick u32, count−1 u4, slots u10, hasAuth u1 [lastAuthTick u32],
//               then for each slot in `slots` ascending, count coded inputs
//               (each against the previous one of that slot, the first
//               against idle)
//   AuthInputs  startTick u32, count−1 u4, hasMargin u1 [margin i8],
//               hasChecksum u1 [back u8, checksum u32] (checksumTick =
//               endTick − back), then per frame: present u10 for the first
//               frame, else samePresent u1 [present u10]; then a coded input
//               per present slot ascending, against that slot's input in the
//               previous frame when it was present there, else idle
//   Snapshot    MatchConfig, worldChecksum u32, WorldState (visitor)
//   Hello       version u16 (frozen), contentHash u32, buildId u32,
//               tokenLength u8, token bytes, requestedSlots u8,
//               nicknameLength u8, nickname bytes (since version 6)
//   Welcome     assignedSlots u10, MatchConfig, serverTick u32
//   Reject      reason u8
//   JoinMatch   requestedSlots u8
//   PlayerJoined slot u8, tick u32, yours u1
//   PlayerLeft  slot u8, tick u32
//   MatchConfig MatchConfig, startTick u32
//   (MatchConfig = seed u64, players u10, arena u8, mode u8, powerUps u16,
//   loadout u8, teams 10×u4; arena and mode below kMaxArenas / kMaxModes,
//   since protocol version 2; powerUps since version 3; loadout below
//   kMaxLoadouts since version 4; teams, each below kMaxPlayers, since
//   version 5)
//   Disconnect  reason u8
//   Ping        sendNs u64, tick u32
//   Pong        echoSendNs u64, tick u32, holdNs u32
//   ResyncRequest tick u32, checksum u32
//   (version 6, the room:)
//   RoomSettings RoomSettings
//   RoomState   phase u2, hasHost u1 [host u4], RoomSettings, humans u10,
//               bots u10, hasWinner u1 [winner u4], then for each human slot
//               ascending: nicknameLength u5, nickname bytes
//   Start       (nothing)
//   (RoomSettings = arena u8, mode u8, loadout u8, powerUps u16, bots u4;
//   arena, mode and loadout below their capacities, bots at most
//   kMaxPlayers)
//   (a nickname: at most kMaxNicknameBytes bytes, validNickname())
#include "kfm/net/messages.hpp"

#include "kfm/core/bitstream.hpp"
#include "kfm/sim/step.hpp"
#include "kfm/sim/visit.hpp"

#include <limits>
#include <utility>

namespace kfm::net {
namespace {

constexpr int kMaskBits = sim::kMaxPlayers;
constexpr int kCountBits = 4;
static_assert((1u << kCountBits) == kMaxInputRedundancy);
static_assert(kMessageTypeCount == static_cast<uint8_t>(MessageType::Start));
static_assert(kRoomPhaseCount == static_cast<uint8_t>(RoomPhase::Over) + 1);
static_assert(kRejectReasonCount == static_cast<uint8_t>(RejectReason::ServerShuttingDown) + 1);
static_assert(kDisconnectReasonCount ==
              static_cast<uint8_t>(DisconnectReason::ServerShuttingDown) + 1);

using std::unexpected;

[[nodiscard]] constexpr bool hasSlot(sim::PlayerMask m, uint16_t slot) {
  return ((static_cast<uint32_t>(m) >> slot) & 1u) != 0;
}

[[nodiscard]] constexpr bool validMask(sim::PlayerMask m) {
  return (m & ~kAllPlayersMask) == 0;
}

[[nodiscard]] constexpr bool validWindow(Tick start, uint8_t count) {
  return count >= 1 && count <= kMaxInputRedundancy &&
         start.value <= std::numeric_limits<Tick::rep>::max() - count;
}

// Runs `body` on a writer over `out` after the type byte, then pads. `body`
// returns false for a message that must not be sent.
template <class F>
[[nodiscard]] size_t encodeWith(std::span<std::byte> out, MessageType type, F&& body) {
  BitWriter w(out);
  w.writeU8(static_cast<uint8_t>(type));
  if (!std::forward<F>(body)(w)) return 0;
  w.align();
  return w.ok() ? w.bytesWritten() : 0;
}

// Checks the type byte, runs `body`, then requires the rest to be zero
// padding shorter than a byte.
template <class F>
[[nodiscard]] DecodeResult decodeWith(std::span<const std::byte> bytes, MessageType type,
                                      F&& body) {
  if (bytes.empty()) return unexpected(DecodeError::Truncated);
  BitReader r(bytes);
  if (r.readU8() != static_cast<uint8_t>(type)) return unexpected(DecodeError::WrongType);
  const DecodeResult res = std::forward<F>(body)(r);
  if (!r.ok()) return unexpected(DecodeError::Truncated);
  if (!res) return res;
  const size_t rest = r.bitsRemaining();
  if (rest >= 8) return unexpected(DecodeError::TrailingBytes);
  if (rest > 0 && r.readBits(static_cast<int>(rest)) != 0) {
    return unexpected(DecodeError::TrailingBytes);
  }
  return {};
}

void writeTick(BitWriter& w, Tick t) {
  w.writeU32(t.value);
}
[[nodiscard]] Tick readTick(BitReader& r) {
  return Tick{r.readU32()};
}

void writeMask(BitWriter& w, sim::PlayerMask m) {
  w.writeBits(m, kMaskBits);
}
[[nodiscard]] sim::PlayerMask readMask(BitReader& r) {
  return static_cast<sim::PlayerMask>(r.readBits(kMaskBits));
}

// v0.32 (protocol 5): a team per slot, 4 bits each (below kMaxPlayers).
constexpr uint32_t kTeamBits = 4;
static_assert(sim::kMaxPlayers <= (1u << kTeamBits));

// The match config every host shares (§6.6): arena and mode are indices
// into the content both sides hashed in Hello, so only the capacity bound
// can be checked here.
[[nodiscard]] bool writeMatchConfig(BitWriter& w, const sim::MatchConfig& c) {
  if (!validMask(c.players) || c.arena >= sim::kMaxArenas || c.mode >= sim::kMaxModes ||
      c.loadout >= sim::kMaxLoadouts) {
    return false;
  }
  w.writeU64(c.seed);
  writeMask(w, c.players);
  w.writeU8(c.arena);
  w.writeU8(c.mode);
  w.writeU16(c.powerUps);
  w.writeU8(c.loadout);
  for (const uint8_t t : c.teams) {
    if (t >= sim::kMaxPlayers) return false;
    w.writeBits(t, kTeamBits);
  }
  return true;
}
[[nodiscard]] bool readMatchConfig(BitReader& r, sim::MatchConfig& c) {
  c.seed = r.readU64();
  c.players = readMask(r);
  c.arena = r.readU8();
  c.mode = r.readU8();
  c.powerUps = r.readU16();
  c.loadout = r.readU8();
  bool teamsOk = true;
  for (uint8_t& t : c.teams) {
    t = static_cast<uint8_t>(r.readBits(kTeamBits));
    teamsOk = teamsOk && t < sim::kMaxPlayers;
  }
  return c.arena < sim::kMaxArenas && c.mode < sim::kMaxModes && c.loadout < sim::kMaxLoadouts &&
         teamsOk;
}

constexpr int kSlotBits = 4;
static_assert(sim::kMaxPlayers <= (1u << kSlotBits));
constexpr int kNicknameLengthBits = 5;
static_assert(kMaxNicknameBytes < (1u << kNicknameLengthBits));
constexpr int kBotCountBits = 4;

[[nodiscard]] bool validSettings(const RoomSettings& s) {
  return s.arena < sim::kMaxArenas && s.mode < sim::kMaxModes && s.loadout < sim::kMaxLoadouts &&
         s.bots <= sim::kMaxPlayers;
}
void writeSettings(BitWriter& w, const RoomSettings& s) {
  w.writeU8(s.arena);
  w.writeU8(s.mode);
  w.writeU8(s.loadout);
  w.writeU16(s.powerUps);
  w.writeBits(s.bots, kBotCountBits);
}
void readSettings(BitReader& r, RoomSettings& s) {
  s.arena = r.readU8();
  s.mode = r.readU8();
  s.loadout = r.readU8();
  s.powerUps = r.readU16();
  s.bots = static_cast<uint8_t>(r.readBits(kBotCountBits));
}

void writeNickname(BitWriter& w, const Nickname& n, int lengthBits) {
  w.writeBits(n.length, lengthBits);
  for (uint8_t i = 0; i < n.length; ++i)
    w.writeU8(static_cast<uint8_t>(n.bytes[i]));
}
// False for a nickname the wire does not accept.
[[nodiscard]] bool readNickname(BitReader& r, Nickname& n, int lengthBits) {
  n = Nickname{};
  const uint32_t len = r.readBits(lengthBits);
  if (len > kMaxNicknameBytes) return false;
  n.length = static_cast<uint8_t>(len);
  for (uint8_t i = 0; i < n.length; ++i)
    n.bytes[i] = static_cast<char>(r.readU8());
  return r.ok() ? validNickname(n.view()) : true;  // a truncated read fails as Truncated
}

// The length in bytes of the well-formed UTF-8 character at the start of
// `s` that is not a control character, or 0.
[[nodiscard]] size_t nicknameCharLength(std::string_view s) {
  const auto b0 = static_cast<uint8_t>(s[0]);
  if (b0 < 0x20 || b0 == 0x7F) return 0;  // C0 controls, DEL
  if (b0 < 0x80) return 1;
  size_t len = 0;
  uint32_t cp = 0;
  uint32_t min = 0;
  if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1Fu;
    min = 0x80;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0Fu;
    min = 0x800;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07u;
    min = 0x10000;
  } else {
    return 0;  // a continuation byte or 0xF8..0xFF
  }
  if (s.size() < len) return 0;
  for (size_t i = 1; i < len; ++i) {
    const auto b = static_cast<uint8_t>(s[i]);
    if ((b & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (b & 0x3Fu);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;  // overlong, surrogate
  if (cp >= 0x80 && cp <= 0x9F) return 0;                                     // C1 controls
  return len;
}

}  // namespace

// --- nicknames --------------------------------------------------------------------

bool validNickname(std::string_view text) {
  if (text.size() > kMaxNicknameBytes) return false;
  while (!text.empty()) {
    const size_t n = nicknameCharLength(text);
    if (n == 0) return false;
    text.remove_prefix(n);
  }
  return true;
}

Nickname Nickname::from(std::string_view text) {
  Nickname out;
  bool pendingSpace = false;  // spaces are kept only between other characters
  while (!text.empty()) {
    const size_t n = nicknameCharLength(text);
    if (n == 0) {
      text.remove_prefix(1);  // a control byte or malformed UTF-8: dropped
      continue;
    }
    if (n == 1 && text[0] == ' ') {
      pendingSpace = out.length > 0;
      text.remove_prefix(1);
      continue;
    }
    const size_t need = n + (pendingSpace ? 1 : 0);
    if (out.length + need > kMaxNicknameBytes) break;
    if (pendingSpace) out.bytes[out.length++] = ' ';
    pendingSpace = false;
    for (size_t i = 0; i < n; ++i)
      out.bytes[out.length++] = text[i];
    text.remove_prefix(n);
  }
  return out;
}

// --- input delta coding ---------------------------------------------------------

void writeInputDelta(BitWriter& w, const sim::PlayerInput& prev, const sim::PlayerInput& in) {
  const std::array<uint8_t, 5> a{prev.moveDir, prev.moveMag, prev.aimDir, prev.buttons,
                                 prev.subTick};
  const std::array<uint8_t, 5> b{in.moveDir, in.moveMag, in.aimDir, in.buttons, in.subTick};
  uint32_t changed = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    if (a[i] != b[i]) changed |= 1u << i;
  }
  w.writeBool(changed == 0);
  if (changed == 0) return;
  w.writeBits(changed, 5);
  for (uint32_t i = 0; i < 5; ++i) {
    if (((changed >> i) & 1u) != 0) w.writeU8(b[i]);
  }
}

void readInputDelta(BitReader& r, const sim::PlayerInput& prev, sim::PlayerInput& out) {
  std::array<uint8_t, 5> v{prev.moveDir, prev.moveMag, prev.aimDir, prev.buttons, prev.subTick};
  if (!r.readBool()) {
    // A zero mask is a valid (if wasteful) "nothing changed"; it re-encodes
    // as the one-bit form, which decodes to the same value.
    const uint32_t changed = r.readBits(5);
    for (uint32_t i = 0; i < 5; ++i) {
      if (((changed >> i) & 1u) != 0) v[i] = r.readU8();
    }
  }
  out = sim::PlayerInput{v[0], v[1], v[2], v[3], v[4]};
}

// --- equality -------------------------------------------------------------------

bool InputMsg::operator==(const InputMsg& o) const {
  if (startTick != o.startTick || count != o.count || slots != o.slots ||
      lastAuthTick != o.lastAuthTick) {
    return false;
  }
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (!hasSlot(slots, s)) continue;
    for (uint8_t i = 0; i < count && i < kMaxInputRedundancy; ++i) {
      if (inputs[s][i] != o.inputs[s][i]) return false;
    }
  }
  return true;
}

bool AuthInputsMsg::operator==(const AuthInputsMsg& o) const {
  if (startTick != o.startTick || count != o.count || arrivalMargin != o.arrivalMargin ||
      checksumTick != o.checksumTick) {
    return false;
  }
  if (checksumTick && checksum != o.checksum) return false;
  for (uint8_t i = 0; i < count && i < kMaxInputRedundancy; ++i) {
    if (!(frames[i] == o.frames[i])) return false;
  }
  return true;
}

bool HelloMsg::operator==(const HelloMsg& o) const {
  if (protocolVersion != o.protocolVersion || contentHash != o.contentHash ||
      buildId != o.buildId || tokenLength != o.tokenLength || requestedSlots != o.requestedSlots) {
    return false;
  }
  for (uint8_t i = 0; i < tokenLength && i < kMaxTokenBytes; ++i) {
    if (token[i] != o.token[i]) return false;
  }
  return nickname == o.nickname;
}

bool RoomStateMsg::operator==(const RoomStateMsg& o) const {
  if (phase != o.phase || host != o.host || settings != o.settings || humans != o.humans ||
      bots != o.bots || winner != o.winner) {
    return false;
  }
  for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
    if (hasSlot(humans, s) && !(names[s] == o.names[s])) return false;
  }
  return true;
}

// --- peek -----------------------------------------------------------------------

Decoded<MessageType> peekType(std::span<const std::byte> bytes) {
  if (bytes.empty()) return unexpected(DecodeError::Truncated);
  const auto t = static_cast<uint8_t>(bytes[0]);
  if (t < 1 || t > kMessageTypeCount) return unexpected(DecodeError::WrongType);
  return static_cast<MessageType>(t);
}

Decoded<uint16_t> peekHelloVersion(std::span<const std::byte> bytes) {
  if (bytes.empty()) return unexpected(DecodeError::Truncated);
  BitReader r(bytes);
  if (r.readU8() != static_cast<uint8_t>(MessageType::Hello)) {
    return unexpected(DecodeError::WrongType);
  }
  const uint16_t v = r.readU16();
  if (!r.ok()) return unexpected(DecodeError::Truncated);
  return v;
}

// --- Input ----------------------------------------------------------------------

size_t encode(const InputMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Input, [&](BitWriter& w) {
    if (!validWindow(m.startTick, m.count) || m.slots == 0 || !validMask(m.slots)) return false;
    writeTick(w, m.startTick);
    w.writeBits(m.count - 1u, kCountBits);
    writeMask(w, m.slots);
    w.writeBool(m.lastAuthTick.has_value());
    if (m.lastAuthTick) writeTick(w, *m.lastAuthTick);
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      if (!hasSlot(m.slots, s)) continue;
      sim::PlayerInput prev{};
      for (uint8_t i = 0; i < m.count; ++i) {
        writeInputDelta(w, prev, m.inputs[s][i]);
        prev = m.inputs[s][i];
      }
    }
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, InputMsg& out) {
  return decodeWith(bytes, MessageType::Input, [&](BitReader& r) -> DecodeResult {
    out.startTick = readTick(r);
    out.count = static_cast<uint8_t>(r.readBits(kCountBits) + 1u);
    out.slots = readMask(r);
    out.lastAuthTick.reset();
    if (r.readBool()) out.lastAuthTick = readTick(r);
    if (!validWindow(out.startTick, out.count) || out.slots == 0) {
      return unexpected(DecodeError::OutOfRange);
    }
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      out.inputs[s] = {};
      if (!hasSlot(out.slots, s)) continue;
      sim::PlayerInput prev{};
      for (uint8_t i = 0; i < out.count; ++i) {
        readInputDelta(r, prev, out.inputs[s][i]);
        prev = out.inputs[s][i];
      }
    }
    return {};
  });
}

// --- AuthInputs -----------------------------------------------------------------

size_t encode(const AuthInputsMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::AuthInputs, [&](BitWriter& w) {
    if (!validWindow(m.startTick, m.count)) return false;
    const Tick end = m.endTick();
    uint32_t back = 0;
    if (m.checksumTick) {
      if (*m.checksumTick > end || end.minus(*m.checksumTick) > 255) return false;
      back = static_cast<uint32_t>(end.minus(*m.checksumTick));
    }
    for (uint8_t i = 0; i < m.count; ++i) {
      if (!validMask(m.frames[i].present)) return false;
    }
    writeTick(w, m.startTick);
    w.writeBits(m.count - 1u, kCountBits);
    w.writeBool(m.arrivalMargin.has_value());
    if (m.arrivalMargin) w.writeI8(*m.arrivalMargin);
    w.writeBool(m.checksumTick.has_value());
    if (m.checksumTick) {
      w.writeBits(back, 8);
      w.writeU32(m.checksum);
    }
    for (uint8_t i = 0; i < m.count; ++i) {
      const sim::InputFrame& f = m.frames[i];
      if (i == 0) {
        writeMask(w, f.present);
      } else {
        const bool same = f.present == m.frames[i - 1].present;
        w.writeBool(same);
        if (!same) writeMask(w, f.present);
      }
      for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (!hasSlot(f.present, s)) continue;
        const bool prevHere = i > 0 && hasSlot(m.frames[i - 1].present, s);
        const sim::PlayerInput prev = prevHere ? m.frames[i - 1].inputs[s] : sim::PlayerInput{};
        writeInputDelta(w, prev, f.inputs[s]);
      }
    }
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, AuthInputsMsg& out) {
  return decodeWith(bytes, MessageType::AuthInputs, [&](BitReader& r) -> DecodeResult {
    out.startTick = readTick(r);
    out.count = static_cast<uint8_t>(r.readBits(kCountBits) + 1u);
    if (!validWindow(out.startTick, out.count)) return unexpected(DecodeError::OutOfRange);
    out.arrivalMargin.reset();
    if (r.readBool()) out.arrivalMargin = r.readI8();
    out.checksumTick.reset();
    out.checksum = 0;
    if (r.readBool()) {
      const uint32_t back = r.readBits(8);
      const Tick end = out.endTick();
      if (back > end.value) return unexpected(DecodeError::OutOfRange);
      out.checksumTick = Tick{end.value - back};
      out.checksum = r.readU32();
    }
    for (uint8_t i = 0; i < kMaxInputRedundancy; ++i) {
      sim::InputFrame& f = out.frames[i];
      f = sim::InputFrame{};
      if (i >= out.count) continue;
      f.tick = out.startTick.plus(i);
      if (i == 0 || !r.readBool()) {
        f.present = readMask(r);
      } else {
        f.present = out.frames[i - 1].present;
      }
      for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
        if (!hasSlot(f.present, s)) continue;
        const bool prevHere = i > 0 && hasSlot(out.frames[i - 1].present, s);
        const sim::PlayerInput prev = prevHere ? out.frames[i - 1].inputs[s] : sim::PlayerInput{};
        readInputDelta(r, prev, f.inputs[s]);
      }
    }
    return {};
  });
}

// --- Ping / Pong ------------------------------------------------------------------

size_t encode(const PingMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Ping, [&](BitWriter& w) {
    w.writeU64(m.sendNs);
    writeTick(w, m.tick);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, PingMsg& out) {
  return decodeWith(bytes, MessageType::Ping, [&](BitReader& r) -> DecodeResult {
    out.sendNs = r.readU64();
    out.tick = readTick(r);
    return {};
  });
}

size_t encode(const PongMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Pong, [&](BitWriter& w) {
    w.writeU64(m.echoSendNs);
    writeTick(w, m.tick);
    w.writeU32(m.holdNs);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, PongMsg& out) {
  return decodeWith(bytes, MessageType::Pong, [&](BitReader& r) -> DecodeResult {
    out.echoSendNs = r.readU64();
    out.tick = readTick(r);
    out.holdNs = r.readU32();
    return {};
  });
}

// --- Snapshot ---------------------------------------------------------------------

size_t encode(const SnapshotMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Snapshot, [&](BitWriter& w) {
    if (!writeMatchConfig(w, m.config)) return false;
    w.writeU32(sim::checksum(m.world));
    return sim::writeTo(w, m.world);
  });
}

DecodeResult decode(std::span<const std::byte> bytes, SnapshotMsg& out) {
  return decodeWith(bytes, MessageType::Snapshot, [&](BitReader& r) -> DecodeResult {
    if (!readMatchConfig(r, out.config)) return unexpected(DecodeError::OutOfRange);
    const uint32_t sum = r.readU32();
    if (!sim::readFrom(r, out.world)) {
      return unexpected(r.ok() ? DecodeError::InvalidState : DecodeError::Truncated);
    }
    if (sim::checksum(out.world) != sum) return unexpected(DecodeError::ChecksumMismatch);
    return {};
  });
}

// --- control ----------------------------------------------------------------------

size_t encode(const HelloMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Hello, [&](BitWriter& w) {
    if (m.tokenLength > kMaxTokenBytes || m.requestedSlots > sim::kMaxPlayers ||
        m.nickname.length > kMaxNicknameBytes || !validNickname(m.nickname.view())) {
      return false;
    }
    w.writeU16(m.protocolVersion);
    w.writeU32(m.contentHash);
    w.writeU32(m.buildId);
    w.writeU8(m.tokenLength);
    w.writeBytes(m.tokenBytes());
    w.writeU8(m.requestedSlots);
    writeNickname(w, m.nickname, 8);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, HelloMsg& out) {
  return decodeWith(bytes, MessageType::Hello, [&](BitReader& r) -> DecodeResult {
    out.protocolVersion = r.readU16();
    out.contentHash = r.readU32();
    out.buildId = r.readU32();
    out.tokenLength = r.readU8();
    if (out.tokenLength > kMaxTokenBytes) return unexpected(DecodeError::OutOfRange);
    out.token = {};
    r.readBytes(std::span<std::byte>(out.token.data(), out.tokenLength));
    out.requestedSlots = r.readU8();
    if (out.requestedSlots > sim::kMaxPlayers) return unexpected(DecodeError::OutOfRange);
    if (!readNickname(r, out.nickname, 8)) return unexpected(DecodeError::OutOfRange);
    return {};
  });
}

size_t encode(const WelcomeMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Welcome, [&](BitWriter& w) {
    if (!validMask(m.assignedSlots)) return false;
    writeMask(w, m.assignedSlots);
    if (!writeMatchConfig(w, m.config)) return false;
    writeTick(w, m.serverTick);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, WelcomeMsg& out) {
  return decodeWith(bytes, MessageType::Welcome, [&](BitReader& r) -> DecodeResult {
    out.assignedSlots = readMask(r);
    if (!readMatchConfig(r, out.config)) return unexpected(DecodeError::OutOfRange);
    out.serverTick = readTick(r);
    return {};
  });
}

size_t encode(const RejectMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Reject, [&](BitWriter& w) {
    const auto reason = static_cast<uint8_t>(m.reason);
    if (reason >= kRejectReasonCount) return false;
    w.writeU8(reason);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, RejectMsg& out) {
  return decodeWith(bytes, MessageType::Reject, [&](BitReader& r) -> DecodeResult {
    const uint8_t reason = r.readU8();
    if (reason >= kRejectReasonCount) return unexpected(DecodeError::OutOfRange);
    out.reason = static_cast<RejectReason>(reason);
    return {};
  });
}

size_t encode(const JoinMatchMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::JoinMatch, [&](BitWriter& w) {
    if (m.requestedSlots > sim::kMaxPlayers) return false;
    w.writeU8(m.requestedSlots);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, JoinMatchMsg& out) {
  return decodeWith(bytes, MessageType::JoinMatch, [&](BitReader& r) -> DecodeResult {
    out.requestedSlots = r.readU8();
    if (out.requestedSlots > sim::kMaxPlayers) return unexpected(DecodeError::OutOfRange);
    return {};
  });
}

size_t encode(const PlayerJoinedMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::PlayerJoined, [&](BitWriter& w) {
    if (m.slot.value >= sim::kMaxPlayers) return false;
    w.writeU8(m.slot.value);
    writeTick(w, m.tick);
    w.writeBool(m.yours);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, PlayerJoinedMsg& out) {
  return decodeWith(bytes, MessageType::PlayerJoined, [&](BitReader& r) -> DecodeResult {
    out.slot = PlayerId{r.readU8()};
    out.tick = readTick(r);
    out.yours = r.readBool();
    if (out.slot.value >= sim::kMaxPlayers) return unexpected(DecodeError::OutOfRange);
    return {};
  });
}

size_t encode(const PlayerLeftMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::PlayerLeft, [&](BitWriter& w) {
    if (m.slot.value >= sim::kMaxPlayers) return false;
    w.writeU8(m.slot.value);
    writeTick(w, m.tick);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, PlayerLeftMsg& out) {
  return decodeWith(bytes, MessageType::PlayerLeft, [&](BitReader& r) -> DecodeResult {
    out.slot = PlayerId{r.readU8()};
    out.tick = readTick(r);
    if (out.slot.value >= sim::kMaxPlayers) return unexpected(DecodeError::OutOfRange);
    return {};
  });
}

size_t encode(const MatchConfigMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::MatchConfig, [&](BitWriter& w) {
    if (!writeMatchConfig(w, m.config)) return false;
    writeTick(w, m.startTick);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, MatchConfigMsg& out) {
  return decodeWith(bytes, MessageType::MatchConfig, [&](BitReader& r) -> DecodeResult {
    if (!readMatchConfig(r, out.config)) return unexpected(DecodeError::OutOfRange);
    out.startTick = readTick(r);
    return {};
  });
}

size_t encode(const DisconnectMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Disconnect, [&](BitWriter& w) {
    const auto reason = static_cast<uint8_t>(m.reason);
    if (reason >= kDisconnectReasonCount) return false;
    w.writeU8(reason);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, DisconnectMsg& out) {
  return decodeWith(bytes, MessageType::Disconnect, [&](BitReader& r) -> DecodeResult {
    const uint8_t reason = r.readU8();
    if (reason >= kDisconnectReasonCount) return unexpected(DecodeError::OutOfRange);
    out.reason = static_cast<DisconnectReason>(reason);
    return {};
  });
}

size_t encode(const ResyncRequestMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::ResyncRequest, [&](BitWriter& w) {
    writeTick(w, m.tick);
    w.writeU32(m.checksum);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, ResyncRequestMsg& out) {
  return decodeWith(bytes, MessageType::ResyncRequest, [&](BitReader& r) -> DecodeResult {
    out.tick = readTick(r);
    out.checksum = r.readU32();
    return {};
  });
}

// --- the room (v6) -----------------------------------------------------------------

size_t encode(const RoomSettingsMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::RoomSettings, [&](BitWriter& w) {
    if (!validSettings(m.settings)) return false;
    writeSettings(w, m.settings);
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, RoomSettingsMsg& out) {
  return decodeWith(bytes, MessageType::RoomSettings, [&](BitReader& r) -> DecodeResult {
    readSettings(r, out.settings);
    if (!validSettings(out.settings)) return unexpected(DecodeError::OutOfRange);
    return {};
  });
}

size_t encode(const RoomStateMsg& m, std::span<std::byte> out) {
  return encodeWith(out, MessageType::RoomState, [&](BitWriter& w) {
    const auto phase = static_cast<uint8_t>(m.phase);
    const bool hasHost = m.host.isValid();
    const bool hasWinner = m.winner.isValid();
    if (phase >= kRoomPhaseCount || !validSettings(m.settings) || !validMask(m.humans) ||
        !validMask(m.bots) || (m.humans & m.bots) != 0 ||
        (hasHost && (m.host.value >= sim::kMaxPlayers || !hasSlot(m.humans, m.host.value))) ||
        (hasWinner && m.winner.value >= sim::kMaxPlayers)) {
      return false;
    }
    w.writeBits(phase, 2);
    w.writeBool(hasHost);
    if (hasHost) w.writeBits(m.host.value, kSlotBits);
    writeSettings(w, m.settings);
    writeMask(w, m.humans);
    writeMask(w, m.bots);
    w.writeBool(hasWinner);
    if (hasWinner) w.writeBits(m.winner.value, kSlotBits);
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      if (!hasSlot(m.humans, s)) continue;
      const Nickname& n = m.names[s];
      if (n.length > kMaxNicknameBytes || !validNickname(n.view())) return false;
      writeNickname(w, n, kNicknameLengthBits);
    }
    return true;
  });
}

DecodeResult decode(std::span<const std::byte> bytes, RoomStateMsg& out) {
  return decodeWith(bytes, MessageType::RoomState, [&](BitReader& r) -> DecodeResult {
    const uint32_t phase = r.readBits(2);
    out.phase = static_cast<RoomPhase>(phase < kRoomPhaseCount ? phase : 0);
    out.host = PlayerId::invalid();
    if (r.readBool()) out.host = PlayerId{static_cast<PlayerId::rep>(r.readBits(kSlotBits))};
    readSettings(r, out.settings);
    out.humans = readMask(r);
    out.bots = readMask(r);
    out.winner = PlayerId::invalid();
    if (r.readBool()) out.winner = PlayerId{static_cast<PlayerId::rep>(r.readBits(kSlotBits))};
    if (phase >= kRoomPhaseCount || !validSettings(out.settings) || (out.humans & out.bots) != 0 ||
        (out.host.isValid() &&
         (out.host.value >= sim::kMaxPlayers || !hasSlot(out.humans, out.host.value))) ||
        (out.winner.isValid() && out.winner.value >= sim::kMaxPlayers)) {
      return unexpected(DecodeError::OutOfRange);
    }
    for (uint16_t s = 0; s < sim::kMaxPlayers; ++s) {
      out.names[s] = Nickname{};
      if (!hasSlot(out.humans, s)) continue;
      if (!readNickname(r, out.names[s], kNicknameLengthBits)) {
        return unexpected(DecodeError::OutOfRange);
      }
    }
    return {};
  });
}

size_t encode(const StartMsg& /*m*/, std::span<std::byte> out) {
  return encodeWith(out, MessageType::Start, [](BitWriter& /*w*/) { return true; });
}

DecodeResult decode(std::span<const std::byte> bytes, StartMsg& /*out*/) {
  return decodeWith(bytes, MessageType::Start, [](BitReader& /*r*/) -> DecodeResult { return {}; });
}

}  // namespace kfm::net
