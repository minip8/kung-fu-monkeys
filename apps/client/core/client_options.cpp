#include "client_options.hpp"

#include "kfm/net/messages.hpp"
#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace kfm::client {

namespace {

[[nodiscard]] bool parseU64(std::string_view v, uint64_t& out) {
  int base = 10;
  if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
    v.remove_prefix(2);
    base = 16;
  }
  if (v.empty()) return false;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out, base);
  return ec == std::errc{} && end == v.data() + v.size();
}

constexpr std::string_view kRoomAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr std::size_t kRoomCodeLength = 5;
constexpr std::size_t kMaxRegionLength = 16;

[[nodiscard]] bool isWsUrl(std::string_view v) {
  return v.starts_with("ws://") || v.starts_with("wss://");
}

[[nodiscard]] char upper(char c) {
  return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

}  // namespace

#ifndef KFM_DEFAULT_LOBBY_URL
#define KFM_DEFAULT_LOBBY_URL ""
#endif

#ifndef KFM_DEFAULT_PAGE_URL
#define KFM_DEFAULT_PAGE_URL ""
#endif

const char* defaultLobbyUrl() {
  return KFM_DEFAULT_LOBBY_URL;
}

const char* defaultPageUrl() {
  return KFM_DEFAULT_PAGE_URL;
}

bool isRoomCodeChar(char c) {
  return kRoomAlphabet.contains(upper(c));
}

std::optional<std::string> normalizeRoomCode(std::string_view code) {
  if (code.size() != kRoomCodeLength) return std::nullopt;
  std::string out;
  for (const char c : code) {
    if (!isRoomCodeChar(c)) return std::nullopt;
    out.push_back(upper(c));
  }
  return out;
}

bool isRegionLabel(std::string_view region) {
  if (region.empty() || region.size() > kMaxRegionLength) return false;
  return std::ranges::all_of(
      region, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::optional<LobbyTarget> lobbyTarget(const ClientOptions& options) {
  if (!options.lobbyMode()) return std::nullopt;
  LobbyTarget t;
  t.create = options.room.empty();
  t.region = options.region;
  t.code = options.room;
  return t;
}

std::string roomShareText(const LobbyRoomInfo& room, std::string_view pageUrl,
                          std::string_view lobbyUrl) {
  if (pageUrl.empty()) {
    std::string s;
    if (!lobbyUrl.empty()) s += "--lobby " + std::string(lobbyUrl) + " ";
    return s + "--room " + room.code;
  }
  // The page without its query and fragment, then ?room=CODE; a ?lobby=
  // the page was opened with goes along (a development lobby), anything
  // else (create, region, bots, ...) stays behind.
  const std::size_t cut = pageUrl.find_first_of("?#");
  std::string link(pageUrl.substr(0, cut));
  link += "?room=" + room.code;
  if (cut != std::string_view::npos && pageUrl[cut] == '?') {
    std::string_view query = pageUrl.substr(cut + 1);
    query = query.substr(0, query.find('#'));
    while (!query.empty()) {
      const std::size_t amp = query.find('&');
      const std::string_view param = query.substr(0, amp);
      if (param.starts_with("lobby=")) link += "&" + std::string(param);
      query.remove_prefix(amp == std::string_view::npos ? query.size() : amp + 1);
    }
  }
  return link;
}

std::string roomInviteLink(const LobbyRoomInfo& room, std::string_view pageUrl) {
  return pageUrl.empty() ? room.code : roomShareText(room, pageUrl, {});
}

ai::BotSpec botSpec(const ClientOptions& options) {
  ai::BotSpec spec{.kind = ai::BotKind::Reference, .tier = options.botDifficulty, .path = {}};
  if (!options.botModule.empty()) {
    spec.kind = ai::BotKind::Module;
    spec.path = options.botModule;
  }
  return spec;
}

std::string randomMonkeyName(uint64_t seed) {
  static constexpr std::array<const char*, 16> kNames{
      "Bonobo", "Macaque", "Mandrill", "Gibbon", "Tamarin", "Capuchin", "Baboon", "Marmoset",
      "Langur", "Howler",  "Colobus",  "Gelada", "Vervet",  "Saki",     "Uakari", "Douc"};
  // splitmix64: neighbouring seeds (clock readings) pick unrelated names.
  uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return std::string(kNames[z % kNames.size()]) + " " + std::to_string(1 + ((z >> 8) % 99));
}

NetSessionConfig netSessionConfig(const ClientOptions& options, bool atStartup) {
  NetSessionConfig c;
  c.nickname = options.nick;
  c.serverLabel = options.lobbyMode() ? options.lobby : options.connect;
  c.lobby = lobbyTarget(options);
  for (const char ch : options.token)
    c.token.push_back(static_cast<std::byte>(ch));
  c.spectate = options.spectate;
  if (options.players != 0) {
    c.requestedSlots = options.players;
  } else {
    c.requestedSlots = atStartup ? 1 : 0;
  }
  return c;
}

bool parseClientOptions(std::span<const char* const> args, ClientOptions& out, std::string& error) {
  const auto value = [&](std::size_t& i, uint64_t& dst) {
    const std::string_view flag = args[i];
    if (i + 1 >= args.size()) {
      error = std::string(flag) + " needs a value";
      return false;
    }
    const std::string_view v = args[++i];
    if (!parseU64(v, dst)) {
      error = std::string(flag) + " needs a non-negative number, got '" + std::string(v) + "'";
      return false;
    }
    return true;
  };

  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--exit-after-frames") {
      if (!value(i, out.exitAfterFrames)) return false;
      if (out.exitAfterFrames == 0) {
        error = "--exit-after-frames needs a positive number";
        return false;
      }
    } else if (arg == "--screenshot") {
      if (i + 1 >= args.size()) {
        error = "--screenshot needs a path";
        return false;
      }
      out.screenshotPath = args[++i];
    } else if (arg == "--autostart") {
      out.autostart = true;
    } else if (arg == "--overlay") {
      out.overlay = true;
    } else if (arg == "--capsules") {
      out.capsules = true;
    } else if (arg == "--mute") {
      out.mute = true;
    } else if (arg == "--verbose") {
      out.verbose = true;
    } else if (arg == "--volume") {
      uint64_t n = 0;
      if (!value(i, n)) return false;
      if (n > 100) {
        error = "--volume must be 0..100";
        return false;
      }
      out.volume = static_cast<uint8_t>(n);
    } else if (arg == "--bots") {
      uint64_t n = 0;
      if (!value(i, n)) return false;
      if (n >= sim::kMaxPlayers) {
        error = "--bots must be below " + std::to_string(sim::kMaxPlayers);
        return false;
      }
      out.bots = static_cast<uint8_t>(n);
    } else if (arg == "--seed") {
      if (!value(i, out.seed)) return false;
    } else if (arg == "--players-seed") {
      if (!value(i, out.playersSeed)) return false;
    } else if (arg == "--bot-difficulty" || arg == "--bot-module" || arg == "--bot") {
      if (i + 1 >= args.size()) {
        error = std::string(arg) + " needs a value";
        return false;
      }
      const std::string_view v = args[++i];
      const std::optional<ai::ReferenceTier> tier = ai::parseReferenceTier(v);
      const std::optional<ai::BotSpec> spec = ai::parseBotSpec(v);
      if (arg == "--bot-difficulty") {
        if (!tier) {
          error = "--bot-difficulty must be easy, medium or hard, got '" + std::string(v) + "'";
          return false;
        }
        out.botDifficulty = *tier;
      } else if (arg == "--bot-module") {
        if (!spec || spec->kind != ai::BotKind::Module) {
          error = "--bot-module needs a .wasm path, got '" + std::string(v) + "'";
          return false;
        }
        out.botModule = spec->path;
      } else if (tier) {
        out.botDifficulty = *tier;
        out.botModule.clear();
      } else if (spec) {
        if (spec->kind == ai::BotKind::Module) {
          out.botModule = spec->path;
        } else {
          out.botDifficulty = spec->tier;
          out.botModule.clear();
        }
      } else {
        error = "--bot needs easy|medium|hard, ref, ref:TIER or a .wasm path, got '" +
                std::string(v) + "'";
        return false;
      }
    } else if (arg == "--content" || arg == "--arena" || arg == "--mode" || arg == "--loadout" ||
               arg == "--powerups") {
      if (i + 1 >= args.size()) {
        error = std::string(arg) + " needs a value";
        return false;
      }
      const std::string_view v = args[++i];
      if (arg == "--content") {
        out.contentDir = v;
      } else if (arg == "--arena") {
        out.arena = v;
      } else if (arg == "--mode") {
        out.mode = v;
      } else if (arg == "--loadout") {
        out.loadout = v;
      } else {
        out.powerUps = v;
      }
    } else if (arg == "--no-hot-reload") {
      out.hotReload = false;
    } else if (arg == "--no-settings") {
      out.settings = false;
    } else if (arg == "--connect" || arg == "--token" || arg == "--stun") {
      if (i + 1 >= args.size()) {
        error = std::string(arg) + " needs a value";
        return false;
      }
      const std::string_view v = args[++i];
      if (arg == "--connect") {
        if (!v.starts_with("ws://") && !v.starts_with("wss://")) {
          error = "--connect needs a ws:// URL, got '" + std::string(v) + "'";
          return false;
        }
        out.connect = v;
      } else if (arg == "--token") {
        if (v.size() > net::kMaxTokenBytes) {
          error = "--token is at most " + std::to_string(net::kMaxTokenBytes) + " bytes";
          return false;
        }
        out.token = v;
      } else {
        out.stun = v;
      }
    } else if (arg == "--nick") {
      if (i + 1 >= args.size()) {
        error = "--nick needs a name";
        return false;
      }
      const net::Nickname n = net::Nickname::from(args[++i]);
      if (n.empty()) {
        error = "--nick needs a name with at least one printable character";
        return false;
      }
      out.nick = n.view();
    } else if (arg == "--lobby" || arg == "--room" || arg == "--region") {
      if (i + 1 >= args.size()) {
        error = std::string(arg) + " needs a value";
        return false;
      }
      const std::string_view v = args[++i];
      if (arg == "--lobby") {
        if (!isWsUrl(v)) {
          error = "--lobby needs a ws:// or wss:// URL, got '" + std::string(v) + "'";
          return false;
        }
        out.lobby = v;
      } else if (arg == "--room") {
        const std::optional<std::string> code = normalizeRoomCode(v);
        if (!code) {
          error =
              "--room needs a 5-character room code (letters and digits, no I, O, 0 or 1), "
              "got '" +
              std::string(v) + "'";
          return false;
        }
        out.room = *code;
      } else {
        if (!isRegionLabel(v)) {
          error =
              "--region needs a region label such as eu, us or asia, got '" + std::string(v) + "'";
          return false;
        }
        out.region = v;
      }
    } else if (arg == "--screen") {
      if (i + 1 >= args.size()) {
        error = "--screen needs a screen";
        return false;
      }
      const std::string_view v = args[++i];
      if (v != "menu" && v != "settings" && v != "online" && v != "local" && v != "paused") {
        error = "--screen must be menu, settings, online, local or paused, got '" + std::string(v) +
                "'";
        return false;
      }
      out.screen = v;
    } else if (arg == "--create") {
      out.create = true;
    } else if (arg == "--spectate") {
      out.spectate = true;
    } else if (arg == "--players") {
      uint64_t n = 0;
      if (!value(i, n)) return false;
      if (n == 0 || n > sim::kMaxPlayers) {
        error = "--players must be 1.." + std::to_string(sim::kMaxPlayers);
        return false;
      }
      out.players = static_cast<uint8_t>(n);
    } else {
      error = "unknown argument '" + std::string(arg) + "'";
      return false;
    }
  }
  if (!out.room.empty() && out.create) {
    error = "--room and --create exclude each other";
    return false;
  }
  if (out.lobbyMode() && !out.connect.empty()) {
    error = std::string(out.create ? "--create" : "--room") +
            " joins through the lobby: it excludes --connect";
    return false;
  }
  if (out.lobbyMode() && out.lobby.empty()) {
    error = std::string(out.create ? "--create" : "--room") +
            " needs a lobby: pass --lobby ws://host:port (this build has no default)";
    return false;
  }
  return true;
}

}  // namespace kfm::client
