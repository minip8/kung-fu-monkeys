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
//
// The same reader and writer serve the lobby's envelopes (docs/lobby-protocol.md
// §1: every lobby message is a flat object of strings) through FlatObject:
// decodeObject() keeps every pair in text order, encodeObject() writes pairs
// with the same escaping. Both apply kMaxMessageBytes to the text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// One key/value pair to encode.
using Field = std::pair<std::string_view, std::string_view>;

// A decoded flat JSON object of strings, its pairs in text order.
struct FlatObject {
  std::vector<std::pair<std::string, std::string>> fields;

  // The value of `key` (the last one when the key repeats, like decode()),
  // or nullopt when absent. The view lives as long as the object.
  [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
};

// {"k1":"v1","k2":"v2",...} in the given order, escaped like encode().
[[nodiscard]] std::string encodeObject(std::span<const Field> fields);
// nullopt unless `text` is one flat object of string values (any keys, none
// included) within kMaxMessageBytes.
[[nodiscard]] std::optional<FlatObject> decodeObject(std::string_view text);

// `url` with `path` appended when it names no path after the authority
// ("ws://h:8080" + "/client" -> "ws://h:8080/client"; "ws://h/x" stays,
// a trailing "/" becomes the path: "ws://h/" -> "ws://h/client"). The lobby
// URLs are given as a base and each side appends its endpoint.
[[nodiscard]] std::string withDefaultPath(std::string_view url, std::string_view path);

}  // namespace kfm::net::signaling
