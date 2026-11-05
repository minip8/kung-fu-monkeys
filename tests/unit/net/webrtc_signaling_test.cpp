#include "kfm/net/webrtc_signaling.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

using namespace kfm::net::signaling;

namespace {

// The decoded message; fails the test when there is none.
Message must(std::string_view text) {
  const auto m = decode(text);
  REQUIRE(m.has_value());
  return m.value_or(Message{});
}

}  // namespace

TEST_CASE("signaling: offer, answer and candidate round-trip", "[net][webrtc][signaling]") {
  Message offer;
  offer.type = Type::Offer;
  offer.sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=\"quoted\" \\ back\t\x01\r\n";
  const std::string text = encode(offer);
  CHECK(text.find('\r') == std::string::npos);
  CHECK(text.find('\n') == std::string::npos);
  const Message back = must(text);
  CHECK(back.type == Type::Offer);
  CHECK(back.sdp == offer.sdp);

  Message answer;
  answer.type = Type::Answer;
  answer.sdp = "v=0\r\n";
  CHECK(must(encode(answer)).type == Type::Answer);

  Message cand;
  cand.type = Type::Candidate;
  cand.candidate = "candidate:1 1 UDP 2122317823 192.168.1.2 50000 typ host";
  cand.mid = "0";
  const Message c = must(encode(cand));
  CHECK(c.type == Type::Candidate);
  CHECK(c.candidate == cand.candidate);
  CHECK(c.mid == "0");
}

TEST_CASE("signaling: the decoder is lenient on layout, strict on content",
          "[net][webrtc][signaling]") {
  // Whitespace, key order, unknown keys, escapes (what a browser's
  // JSON.stringify would produce).
  const Message m =
      must(R"( { "sdp" : "a\u00e9\ud83d\ude00\/b" , "extra":"x", "type":"answer" } )");
  CHECK(m.type == Type::Answer);
  CHECK(m.sdp == "a\xC3\xA9\xF0\x9F\x98\x80/b");

  CHECK_FALSE(decode("").has_value());
  CHECK_FALSE(decode("{}").has_value());
  CHECK_FALSE(decode(R"({"type":"offer"})").has_value());                      // no sdp
  CHECK_FALSE(decode(R"({"type":"candidate","candidate":"c"})").has_value());  // no mid
  CHECK_FALSE(decode(R"({"type":"bye","sdp":"x"})").has_value());
  CHECK_FALSE(decode(R"({"type":"offer","sdp":"x")").has_value());  // unterminated
  CHECK_FALSE(decode(R"({"type":"offer","sdp":"x"} trailing)").has_value());
  CHECK_FALSE(decode(R"({"type":"offer","sdp":1})").has_value());         // not a string
  CHECK_FALSE(decode(R"({"type":"offer","sdp":"\ud83d"})").has_value());  // lone surrogate
  CHECK_FALSE(decode(R"({"type":"offer","sdp":"\q"})").has_value());
  CHECK_FALSE(decode("{\"type\":\"offer\",\"sdp\":\"a\nb\"}").has_value());  // raw control char
  CHECK_FALSE(decode(std::string(kMaxMessageBytes + 1, ' ')).has_value());
}

TEST_CASE("signaling: flat objects round-trip in order", "[net][webrtc][signaling][lobby]") {
  const std::array<Field, 4> fields = {
      Field{"type", "signal"},
      Field{"peer", "7"},
      Field{"msg", R"({"type":"offer","sdp":"v=0\r\n"})"},
      Field{"empty", ""},
  };
  const std::string text = encodeObject(fields);
  CHECK(text.starts_with(R"({"type":"signal","peer":"7","msg":"{\"type\":)"));
  const auto o = decodeObject(text);
  if (!o) {
    FAIL("decodeObject");
    return;
  }
  REQUIRE(o->fields.size() == 4);
  CHECK(o->fields[0].first == "type");
  CHECK(o->fields[3].first == "empty");
  CHECK(o->get("type") == std::optional<std::string_view>("signal"));
  CHECK(o->get("empty") == std::optional<std::string_view>(""));
  CHECK_FALSE(o->get("missing").has_value());
  // The nested signaling message decodes after unwrapping.
  const Message inner = must(o->get("msg").value_or(""));
  CHECK(inner.sdp == "v=0\r\n");

  CHECK(encodeObject({}) == "{}");
  CHECK(decodeObject("{}").has_value());
  CHECK(decodeObject(" { } ").has_value());
}

TEST_CASE("signaling: lobby base URLs get their endpoint path", "[net][webrtc][signaling][lobby]") {
  CHECK(withDefaultPath("ws://127.0.0.1:8080", "/client") == "ws://127.0.0.1:8080/client");
  CHECK(withDefaultPath("wss://lobby.example.com", "/server") == "wss://lobby.example.com/server");
  CHECK(withDefaultPath("wss://lobby.example.com/", "/server") == "wss://lobby.example.com/server");
  CHECK(withDefaultPath("ws://h:1/server", "/server") == "ws://h:1/server");
  CHECK(withDefaultPath("ws://h:1/custom/client", "/client") == "ws://h:1/custom/client");
}

TEST_CASE("signaling: the flat-object decoder is strict like decode()",
          "[net][webrtc][signaling][lobby]") {
  // A repeated key: get() returns the last one, as decode() always did.
  const auto dup = decodeObject(R"({"type":"a","type":"b"})");
  REQUIRE(dup.has_value());
  CHECK(dup.value_or(FlatObject{}).get("type") == std::optional<std::string_view>("b"));

  CHECK_FALSE(decodeObject("").has_value());
  CHECK_FALSE(decodeObject("[]").has_value());
  CHECK_FALSE(decodeObject(R"({"type":"room","n":1})").has_value());  // not a string
  CHECK_FALSE(decodeObject(R"({"type":{"x":"y"}})").has_value());     // nested object
  CHECK_FALSE(decodeObject(R"({"type":"room"} x)").has_value());      // trailing text
  CHECK_FALSE(decodeObject(R"({"type":"room",})").has_value());       // trailing comma
  CHECK_FALSE(decodeObject(R"({"type" "room"})").has_value());        // no colon
  CHECK_FALSE(decodeObject(std::string(kMaxMessageBytes + 1, ' ')).has_value());
  std::string big = R"({"k":")";
  big += std::string(kMaxMessageBytes - big.size() - 2, 'x');
  big += "\"}";
  CHECK(big.size() == kMaxMessageBytes);
  CHECK(decodeObject(big).has_value());
}
