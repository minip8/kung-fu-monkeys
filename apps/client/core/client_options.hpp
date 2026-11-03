// kfm_client command line (headless-testable).
//
//   kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]
//              [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]
//              [--connect ws://host:port [--token T] [--spectate] [--stun URL] [--players N]]
#pragma once

#include "match_session.hpp"
#include "net_session.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace kfm::client {

inline constexpr const char* kUsage =
    "usage: kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]\n"
    "                  [--overlay] [--capsules] [--exit-after-frames N] [--screenshot out.ppm]\n"
    "                  [--connect ws://host:port [--token T] [--spectate] [--stun URL]\n"
    "                   [--players N]]\n"
    "  --autostart            skip the lobby: start the match on the first frame\n"
    "  --bots N               bots filling the slots after the local players (0-9)\n"
    "  --seed S               world seed (MatchConfig::seed)\n"
    "  --players-seed S       seed of the bots' scripted-input Rng\n"
    "  --overlay              open the debug overlay (F1) on startup\n"
    "  --capsules             draw players as capsules, not the skinned model\n"
    "  --exit-after-frames N  render N frames and exit 0 (smoke test)\n"
    "  --screenshot path      write the last frame as binary PPM (native only)\n"
    "network mode:\n"
    "  --connect ws://h:p     play on a server: connects at startup; Esc goes to the\n"
    "                         local lobby, whose Start reconnects with every device\n"
    "  --token T              connection token sent to the server\n"
    "  --spectate             watch without a slot\n"
    "  --stun URL             STUN server (stun:host:port) for ICE\n"
    "  --players N            local slots to request (default: 1 at startup, then\n"
    "                         the devices bound in the lobby when connecting)\n";

struct ClientOptions {
  uint64_t exitAfterFrames{0};  // 0: run until quit
  std::string screenshotPath;   // empty: none
  bool autostart{false};
  bool overlay{false};
  bool capsules{false};  // capsule placeholders instead of the skinned character model
  uint8_t bots{0};
  uint64_t seed{1};
  uint64_t playersSeed{kDefaultBotSeed};
  // Network mode (empty connect: local play).
  std::string connect;  // signaling URL, "ws://host:port"
  std::string token;
  std::string stun;  // "stun:host:port"
  bool spectate{false};
  uint8_t players{0};  // local slots to request; 0 = the bound devices

  [[nodiscard]] bool networkMode() const { return !connect.empty(); }
};

// The NetSession settings of the network flags. `atStartup` is the connect
// at launch, which requests one slot unless --players says otherwise; a
// connect from the lobby requests one per bound device by default.
[[nodiscard]] NetSessionConfig netSessionConfig(const ClientOptions& options, bool atStartup);

// Parses argv[1..]. On failure returns false and sets `error`. Numbers accept
// decimal or 0x-prefixed hex.
[[nodiscard]] bool parseClientOptions(std::span<const char* const> args, ClientOptions& out,
                                      std::string& error);

}  // namespace kfm::client
