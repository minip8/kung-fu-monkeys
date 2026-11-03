#include "kfm/net/webrtc_signaling.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kfm::net::signaling {
namespace {

void appendEscaped(std::string& out, std::string_view s) {
  constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  out.push_back('"');
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (u < 0x20) {
          out += "\\u00";
          out.push_back(kHex[u >> 4U]);
          out.push_back(kHex[u & 0xFU]);
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void appendField(std::string& out, std::string_view key, std::string_view value) {
  out.push_back(',');
  appendEscaped(out, key);
  out.push_back(':');
  appendEscaped(out, value);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
  }
}

// Recursive-descent reader over one flat object of string values.
class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  void skipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r'))
      ++pos_;
  }
  [[nodiscard]] bool consume(char c) {
    skipSpace();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  [[nodiscard]] bool atEnd() {
    skipSpace();
    return pos_ == text_.size();
  }

  [[nodiscard]] std::optional<std::string> string() {
    if (!consume('"')) return std::nullopt;
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) return std::nullopt;
      const char e = text_[pos_++];
      switch (e) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          auto cp = hex4();
          if (!cp) return std::nullopt;
          if (*cp >= 0xD800 && *cp < 0xDC00) {  // high surrogate: a low one must follow
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
              return std::nullopt;
            pos_ += 2;
            const auto lo = hex4();
            if (!lo || *lo < 0xDC00 || *lo >= 0xE000) return std::nullopt;
            cp = 0x10000 + ((*cp - 0xD800) << 10U) + (*lo - 0xDC00);
          } else if (*cp >= 0xDC00 && *cp < 0xE000) {
            return std::nullopt;
          }
          appendUtf8(out, *cp);
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;  // unterminated
  }

 private:
  [[nodiscard]] std::optional<uint32_t> hex4() {
    if (pos_ + 4 > text_.size()) return std::nullopt;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = text_[pos_++];
      uint32_t d = 0;
      if (h >= '0' && h <= '9') {
        d = static_cast<uint32_t>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        d = static_cast<uint32_t>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        d = static_cast<uint32_t>(h - 'A' + 10);
      } else {
        return std::nullopt;
      }
      v = (v << 4U) | d;
    }
    return v;
  }

  std::string_view text_;
  size_t pos_{0};
};

[[nodiscard]] std::string_view typeName(Type type) {
  switch (type) {
    case Type::Offer:
      return "offer";
    case Type::Answer:
      return "answer";
    case Type::Candidate:
      return "candidate";
  }
  return "offer";
}

}  // namespace

std::string encode(const Message& message) {
  std::string out = "{\"type\":";
  appendEscaped(out, typeName(message.type));
  if (message.type == Type::Candidate) {
    appendField(out, "candidate", message.candidate);
    appendField(out, "mid", message.mid);
  } else {
    appendField(out, "sdp", message.sdp);
  }
  out.push_back('}');
  return out;
}

std::optional<Message> decode(std::string_view text) {
  if (text.size() > kMaxMessageBytes) return std::nullopt;
  Reader r(text);
  if (!r.consume('{')) return std::nullopt;
  std::optional<std::string> type;
  std::optional<std::string> sdp;
  std::optional<std::string> candidate;
  std::optional<std::string> mid;
  if (!r.consume('}')) {
    do {
      auto key = r.string();
      if (!key || !r.consume(':')) return std::nullopt;
      auto value = r.string();
      if (!value) return std::nullopt;
      if (*key == "type") {
        type = std::move(value);
      } else if (*key == "sdp") {
        sdp = std::move(value);
      } else if (*key == "candidate") {
        candidate = std::move(value);
      } else if (*key == "mid") {
        mid = std::move(value);
      }
    } while (r.consume(','));
    if (!r.consume('}')) return std::nullopt;
  }
  if (!r.atEnd() || !type) return std::nullopt;

  Message m;
  if (*type == "offer" || *type == "answer") {
    if (!sdp) return std::nullopt;
    m.type = *type == "offer" ? Type::Offer : Type::Answer;
    m.sdp = std::move(*sdp);
  } else if (*type == "candidate") {
    if (!candidate || !mid) return std::nullopt;
    m.type = Type::Candidate;
    m.candidate = std::move(*candidate);
    m.mid = std::move(*mid);
  } else {
    return std::nullopt;
  }
  return m;
}

}  // namespace kfm::net::signaling
