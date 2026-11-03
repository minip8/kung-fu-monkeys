#include "client_options.hpp"

#include "kfm/net/messages.hpp"
#include "kfm/sim/constants.hpp"

#include <charconv>
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

}  // namespace

NetSessionConfig netSessionConfig(const ClientOptions& options, bool atStartup) {
  NetSessionConfig c;
  c.serverLabel = options.connect;
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
  return true;
}

}  // namespace kfm::client
