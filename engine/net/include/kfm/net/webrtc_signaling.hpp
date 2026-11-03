// Signaling messages of WebRtcTransport (architecture §6.4). One WebSocket
// text message per signaling message, each a flat JSON object of strings:
//
//   {"type":"offer","sdp":"v=0\r\n..."}            client -> server
//   {"type":"answer","sdp":"v=0\r\n..."}           server -> client
//   {"type":"candidate","candidate":"candidate:...","mid":"0"}   both ways
//
// Sequence: the client opens ws://host:port, creates its peer connection and
// the channels "r" (reliable, ordered) and "u" (unordered, no retransmits),
// and sends its offer; the server answers. Both sides trickle ICE candidates
// as they are gathered. The data channels are negotiated in-band (the client
// opens them, the server accepts them), so nothing else crosses the socket.
// It stays open for late candidates; closing it after Connected is harmless.
//
// Hand-written so the transport needs no JSON library: the encoder escapes
// '"', '\\' and control characters; the decoder accepts any flat object whose
// values are strings (escapes including \uXXXX and surrogate pairs), ignores
// unknown keys and rejects everything else. It never throws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kfm::net::signaling {

// Longest signaling message either side accepts (an SDP is ~1 KB).
inline constexpr size_t kMaxMessageBytes = size_t{64} * 1024;

enum class Type : uint8_t { Offer = 0, Answer, Candidate };

struct Message {
  Type type{Type::Offer};
  std::string sdp;        // Offer, Answer
  std::string candidate;  // Candidate
  std::string mid;        // Candidate
};

[[nodiscard]] std::string encode(const Message& message);
// nullopt for malformed JSON, an unknown type, a missing field or a message
// over kMaxMessageBytes.
[[nodiscard]] std::optional<Message> decode(std::string_view text);

}  // namespace kfm::net::signaling
