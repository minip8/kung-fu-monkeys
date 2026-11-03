#include "kfm/net/webrtc_signaling.hpp"

#include <catch2/catch_test_macros.hpp>

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
