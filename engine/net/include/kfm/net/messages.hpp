// Message layer (architecture §6.3): the wire format of every netcode
// message. Each message starts with a one-byte MessageType; the rest is
// bit-packed with BitWriter (LSB first, see kfm/core/bitstream.hpp) and padded
// with zero bits to a whole byte. The layout is a wire contract: change it
// only together with kProtocolVersion.
//
// Encoding: encode(msg, out) writes into a caller buffer and returns the bytes
// written, or 0 when the buffer is too small or the message violates a range
// (count, mask, token length), so a malformed message is never sent.
//
// Decoding is fallible and never trusts the input: decode(bytes, out) returns
// std::expected<void, DecodeError>. It checks the type byte, every count,
// mask, slot index, enum and tick window, rejects trailing bytes and non-zero
// padding, and leaves `out` unspecified on failure. Any byte string either
// fails cleanly or decodes to a value that encodes again (fuzzed in
// tests/unit/net/messages_fuzz_test.cpp).
//
// Input coding (Input and AuthInputs): a run of PlayerInputs is coded against
// the previous one in the run (the first against an idle PlayerInput{}): one
// bit "same as previous"; otherwise a 5-bit mask of the changed fields
// (moveDir, moveMag, aimDir, buttons, subTick) followed by those fields, 8 bits
// each. A held stick costs one bit per tick; the worst case is 46 bits.
#pragma once

#include "kfm/core/types.hpp"
#include "kfm/net/transport.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/world_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace kfm {
class BitReader;
class BitWriter;
}  // namespace kfm

namespace kfm::net {

// Bumped on any change to a message layout. Exchanged in Hello.
// v2 (M4): MatchConfig in Welcome, Snapshot and MatchConfig carries arena
// and mode. v3 (M4 lobby): and the per-match power-up set. v4 (v0.31): and
// the loadout. v5 (v0.32): and the team of every slot. v6 (v0.42, M6b):
// the room (RoomSettings, RoomState, Start) and a nickname in Hello. v7:
// RoomState carries the bots' placement (botPlacement), so the waiting room
// shows the slots the server will put its bots in (issue #68).
inline constexpr uint16_t kProtocolVersion = 7;

// Inputs per Input / AuthInputs message: the newest tick and up to 15 before
// it (§6.3: the last 8–16 are resent every packet).
inline constexpr uint8_t kMaxInputRedundancy = 16;
// Connection token from the lobby service, opaque to the game server.
inline constexpr uint8_t kMaxTokenBytes = 64;
// A player's name (v0.42): at most this many bytes of UTF-8.
inline constexpr uint8_t kMaxNicknameBytes = 16;
inline constexpr sim::PlayerMask kAllPlayersMask =
    static_cast<sim::PlayerMask>((1u << sim::kMaxPlayers) - 1u);

enum class MessageType : uint8_t {
  Input = 1,      // C→S unreliable
  AuthInputs,     // S→C unreliable
  Snapshot,       // S→C reliable
  Hello,          // C→S reliable
  Welcome,        // S→C reliable
  Reject,         // S→C reliable
  JoinMatch,      // C→S reliable
  PlayerJoined,   // S→C reliable
  PlayerLeft,     // S→C reliable
  MatchConfig,    // S→C reliable
  Disconnect,     // both, reliable
  Ping,           // both, unreliable
  Pong,           // both, unreliable
  ResyncRequest,  // C→S reliable
  RoomSettings,   // C→S reliable (v6: the host's settings)
  RoomState,      // S→C reliable (v6)
  Start,          // C→S reliable (v6: the host starts or plays again)
};
inline constexpr uint8_t kMessageTypeCount = 17;  // values 1..kMessageTypeCount

enum class DecodeError : uint8_t {
  Truncated = 0,     // ran past the end of the buffer
  WrongType,         // type byte unknown or not the expected message
  OutOfRange,        // a count, mask, slot, enum or tick outside its range
  TrailingBytes,     // bytes or non-zero padding after the message
  InvalidState,      // snapshot world rejected by the visitor (§5.2)
  ChecksumMismatch,  // snapshot world does not match its checksum
};

template <class T>
using Decoded = std::expected<T, DecodeError>;
using DecodeResult = std::expected<void, DecodeError>;

// --- hot path (unreliable) ----------------------------------------------------

// C→S every tick. inputs[slot][i] is the input of player `slot` for tick
// startTick + i, for every slot in `slots` (a client may own several slots:
// local multiplayer behind one connection) and i < count.
struct InputMsg {
  Tick startTick{};
  uint8_t count{1};                  // 1..kMaxInputRedundancy
  sim::PlayerMask slots{0};          // non-empty, within kAllPlayersMask
  std::optional<Tick> lastAuthTick;  // newest authoritative tick the client holds
  std::array<std::array<sim::PlayerInput, kMaxInputRedundancy>, sim::kMaxPlayers> inputs{};

  [[nodiscard]] Tick endTick() const { return startTick.plus(count); }  // exclusive
  [[nodiscard]] bool operator==(const InputMsg& o) const;
};

// S→C every tick: the authoritative frames startTick .. startTick+count-1
// (frames[i].tick == startTick + i). Redundant like InputMsg.
struct AuthInputsMsg {
  Tick startTick{};
  uint8_t count{1};  // 1..kMaxInputRedundancy
  std::array<sim::InputFrame, kMaxInputRedundancy> frames{};
  // inputTick − serverTick of this client's newest input as it arrived
  // (§6.2; positive = early), saturated to int8. Absent before any input.
  std::optional<int8_t> arrivalMargin;
  // checksum(world) of the state whose world.tick == checksumTick, which is
  // at most endTick() and at least endTick() − 255.
  std::optional<Tick> checksumTick;
  uint32_t checksum{0};

  [[nodiscard]] Tick endTick() const { return startTick.plus(count); }  // exclusive
  [[nodiscard]] bool operator==(const AuthInputsMsg& o) const;
};

// Clock sync and RTT (§6.2), either direction. The responder echoes sendNs
// (on the pinger's clock) and reports its own tick and how long it held the
// ping before answering.
struct PingMsg {
  uint64_t sendNs{0};
  Tick tick{};
  constexpr bool operator==(const PingMsg&) const = default;
};
struct PongMsg {
  uint64_t echoSendNs{0};
  Tick tick{};         // responder's current tick when it answered
  uint32_t holdNs{0};  // responder's receipt-to-reply time, saturated
  constexpr bool operator==(const PongMsg&) const = default;
};

// --- reliable -----------------------------------------------------------------

// Full state for join, spectate, reconnect and resync (§6.6). The tick is
// world.tick. Encoded through the visitor with a checksum of the world.
struct SnapshotMsg {
  sim::MatchConfig config{};
  sim::WorldState world{};
};

// A player's name as the wire carries it (v0.42): 0..kMaxNicknameBytes
// bytes of well-formed UTF-8 without control characters (C0, DEL, C1).
// Empty means "no name": hosts show the slot label instead.
struct Nickname {
  std::array<char, kMaxNicknameBytes> bytes{};
  uint8_t length{0};

  [[nodiscard]] std::string_view view() const { return {bytes.data(), length}; }
  [[nodiscard]] bool empty() const { return length == 0; }
  // `text` made valid: control characters and malformed UTF-8 dropped,
  // spaces trimmed at both ends and runs of them made one, cut to
  // kMaxNicknameBytes at a character boundary.
  [[nodiscard]] static Nickname from(std::string_view text);
  [[nodiscard]] bool operator==(const Nickname& o) const { return view() == o.view(); }
};
// Whether `text` is a nickname the wire accepts as is (see Nickname).
[[nodiscard]] bool validNickname(std::string_view text);

struct HelloMsg {
  uint16_t protocolVersion{kProtocolVersion};
  uint32_t contentHash{0};
  uint32_t buildId{0};
  uint8_t tokenLength{0};  // 0..kMaxTokenBytes
  std::array<std::byte, kMaxTokenBytes> token{};
  uint8_t requestedSlots{0};  // local players wanted, 0..kMaxPlayers; 0 = spectate
  Nickname nickname{};        // v6

  [[nodiscard]] std::span<const std::byte> tokenBytes() const {
    return {token.data(), tokenLength};
  }
  [[nodiscard]] bool operator==(const HelloMsg& o) const;
};

struct WelcomeMsg {
  sim::PlayerMask assignedSlots{0};  // empty for a spectator
  sim::MatchConfig config{};
  Tick serverTick{};
  constexpr bool operator==(const WelcomeMsg& o) const {
    return assignedSlots == o.assignedSlots && config == o.config && serverTick == o.serverTick;
  }
};

enum class RejectReason : uint8_t {
  ProtocolMismatch = 0,
  ContentMismatch,
  BuildMismatch,
  BadToken,
  MatchFull,
  ServerShuttingDown,
};
inline constexpr uint8_t kRejectReasonCount = 6;

struct RejectMsg {
  RejectReason reason{RejectReason::ProtocolMismatch};
  constexpr bool operator==(const RejectMsg&) const = default;
};

// A client asks for more local slots after Hello (another local player
// pressed join) or, with 0, gives up to spectating.
struct JoinMatchMsg {
  uint8_t requestedSlots{1};  // 0..kMaxPlayers
  constexpr bool operator==(const JoinMatchMsg&) const = default;
};

// Slot `slot` is present from `tick` on. `yours` is set on the copy sent to
// the client that owns the slot.
struct PlayerJoinedMsg {
  PlayerId slot{};
  Tick tick{};
  bool yours{false};
  constexpr bool operator==(const PlayerJoinedMsg&) const = default;
};

struct PlayerLeftMsg {
  PlayerId slot{};
  Tick tick{};
  constexpr bool operator==(const PlayerLeftMsg&) const = default;
};

// A new match (or restart) starting at startTick with this config.
struct MatchConfigMsg {
  sim::MatchConfig config{};
  Tick startTick{};
  constexpr bool operator==(const MatchConfigMsg& o) const {
    return config == o.config && startTick == o.startTick;
  }
};

enum class DisconnectReason : uint8_t {
  Quit = 0,
  Kicked,
  Timeout,
  ProtocolError,
  ServerShuttingDown,
};
inline constexpr uint8_t kDisconnectReasonCount = 5;

struct DisconnectMsg {
  DisconnectReason reason{DisconnectReason::Quit};
  constexpr bool operator==(const DisconnectMsg&) const = default;
};

// The client's state disagreed with the server's checksum at `tick` (§6.6);
// the server answers with a fresh Snapshot. `checksum` is the client's own,
// for the server's log.
struct ResyncRequestMsg {
  Tick tick{};
  uint32_t checksum{0};
  constexpr bool operator==(const ResyncRequestMsg&) const = default;
};

// --- the room (v6, architecture §6.6) ------------------------------------------

// The online room's state on the server (kept beside ServerHost, never in
// WorldState): Waiting (no match runs; the server does not tick), Playing
// (as before M6b) and Over (the match ended; the server does not tick).
enum class RoomPhase : uint8_t { Waiting = 0, Playing, Over };
inline constexpr uint8_t kRoomPhaseCount = 3;

// What the host picks: the MatchConfig axes (indices into the content both
// sides hashed) and the number of server-side bots.
struct RoomSettings {
  uint8_t arena{0};
  uint8_t mode{0};
  uint8_t loadout{0};
  uint16_t powerUps{sim::kAllPowerUps};
  uint8_t bots{0};  // 0..kMaxPlayers
  constexpr bool operator==(const RoomSettings&) const = default;
};

// Host → server: new settings, in Waiting. The server validates them
// against its content and answers with RoomState either way.
struct RoomSettingsMsg {
  RoomSettings settings{};
  constexpr bool operator==(const RoomSettingsMsg&) const = default;
};

// Host → server: Waiting → Playing with the room's settings; Over →
// Waiting ("play again"). Anyone else's is ignored.
struct StartMsg {
  constexpr bool operator==(const StartMsg&) const = default;
};

// Server → every peer, on every change and after Welcome. `names` holds the
// nickname of every human slot (the others are empty).
struct RoomStateMsg {
  RoomPhase phase{RoomPhase::Waiting};
  PlayerId host{PlayerId::invalid()};  // a human slot, or invalid: no host
  RoomSettings settings{};
  sim::PlayerMask humans{0};  // slots held by peers (reserved ones too)
  sim::PlayerMask bots{0};    // server-side bot slots, disjoint from humans
  // v7: where the room's bots are, disjoint from humans. Waiting: the slots
  // the settings' bots take if the host starts now (the server decides
  // them with the function that places the bots at Start, apps/server
  // roomBotSlots(): the lowest free slots under its player cap, so fewer
  // than settings.bots when fewer are free); no bot holds a slot yet.
  // Playing and Over: the slots bots hold, equal to `bots` (a bot that
  // yields its slot to a joiner leaves both).
  sim::PlayerMask botPlacement{0};
  PlayerId winner{PlayerId::invalid()};  // Over: the match's winner, invalid on a draw
  std::array<Nickname, sim::kMaxPlayers> names{};
  [[nodiscard]] bool operator==(const RoomStateMsg& o) const;
};

// --- sizes --------------------------------------------------------------------

// Upper bounds on the encoded sizes, from the layouts in messages.cpp. Both
// hot-path messages fit one unfragmented datagram at the worst case (10
// players, 16 ticks, every field of every input changing).
inline constexpr size_t kInputDeltaMaxBits = 1 + 5 + 5 * 8;  // one coded PlayerInput
inline constexpr size_t kInputMsgMaxBytes =
    (8 + 32 + 4 + sim::kMaxPlayers + 1 + 32 +
     size_t{sim::kMaxPlayers} * kMaxInputRedundancy * kInputDeltaMaxBits + 7) /
    8;
// A present-mask change costs at most 11 bits but drops a 46-bit input, so
// the worst AuthInputs has every slot present in every frame.
inline constexpr size_t kAuthInputsMsgMaxBytes =
    (8 + 32 + 4 + 1 + 8 + 1 + 8 + 32 + sim::kMaxPlayers + (kMaxInputRedundancy - 1) +
     size_t{kMaxInputRedundancy} * sim::kMaxPlayers * kInputDeltaMaxBits + 7) /
    8;
inline constexpr size_t kPingMsgBytes = 1 + 8 + 4;
inline constexpr size_t kPongMsgBytes = 1 + 8 + 4 + 4;
inline constexpr size_t kRoomSettingsBits = 8 + 8 + 8 + 16 + 4;
inline constexpr size_t kRoomSettingsMsgBytes = (8 + kRoomSettingsBits + 7) / 8;
inline constexpr size_t kStartMsgBytes = 1;
inline constexpr size_t kRoomStateMsgMaxBytes =
    (8 + 2 + 1 + 4 + kRoomSettingsBits + (3 * size_t{sim::kMaxPlayers}) + 1 + 4 +
     size_t{sim::kMaxPlayers} * (5 + kMaxNicknameBytes * 8) + 7) /
    8;
static_assert(kRoomSettingsMsgBytes <= kMaxUnreliableBytes);
static_assert(kRoomStateMsgMaxBytes <= kMaxUnreliableBytes);
static_assert(kInputMsgMaxBytes <= kMaxUnreliableBytes);
static_assert(kAuthInputsMsgMaxBytes <= kMaxUnreliableBytes);

// --- API ----------------------------------------------------------------------

// Type of a message without decoding it.
[[nodiscard]] Decoded<MessageType> peekType(std::span<const std::byte> bytes);
// The protocol version of a Hello. The type byte and this u16 are frozen
// across versions, so a server can Reject(ProtocolMismatch) a Hello whose
// remaining layout it cannot read.
[[nodiscard]] Decoded<uint16_t> peekHelloVersion(std::span<const std::byte> bytes);

// Each returns the bytes written to `out`, 0 on overflow or an invalid message.
[[nodiscard]] size_t encode(const InputMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const AuthInputsMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const PingMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const PongMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const SnapshotMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const HelloMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const WelcomeMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const RejectMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const JoinMatchMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const PlayerJoinedMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const PlayerLeftMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const MatchConfigMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const DisconnectMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const ResyncRequestMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const RoomSettingsMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const RoomStateMsg& m, std::span<std::byte> out);
[[nodiscard]] size_t encode(const StartMsg& m, std::span<std::byte> out);

[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, InputMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, AuthInputsMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, PingMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, PongMsg& out);
// SnapshotMsg is large (a WorldState): decode into a long-lived object.
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, SnapshotMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, HelloMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, WelcomeMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, RejectMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, JoinMatchMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, PlayerJoinedMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, PlayerLeftMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, MatchConfigMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, DisconnectMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, ResyncRequestMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, RoomSettingsMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, RoomStateMsg& out);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes, StartMsg& out);

// The delta coding of one PlayerInput against `prev` (exposed for tests and
// for size estimates).
void writeInputDelta(BitWriter& w, const sim::PlayerInput& prev, const sim::PlayerInput& in);
void readInputDelta(BitReader& r, const sim::PlayerInput& prev, sim::PlayerInput& out);

}  // namespace kfm::net
