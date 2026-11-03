// kfm_client command line (headless-testable).
//
//   kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]
//              [--overlay] [--exit-after-frames N] [--screenshot out.ppm]
#pragma once

#include "match_session.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace kfm::client {

inline constexpr const char* kUsage =
    "usage: kfm_client [--autostart] [--bots N] [--seed S] [--players-seed S]\n"
    "                  [--overlay] [--exit-after-frames N] [--screenshot out.ppm]\n"
    "  --autostart            skip the lobby: start the match on the first frame\n"
    "  --bots N               bots filling the slots after the local players (0-9)\n"
    "  --seed S               world seed (MatchConfig::seed)\n"
    "  --players-seed S       seed of the bots' scripted-input Rng\n"
    "  --overlay              open the debug overlay (F1) on startup\n"
    "  --exit-after-frames N  render N frames and exit 0 (smoke test)\n"
    "  --screenshot path      write the last frame as binary PPM (native only)\n";

struct ClientOptions {
  uint64_t exitAfterFrames{0};  // 0: run until quit
  std::string screenshotPath;   // empty: none
  bool autostart{false};
  bool overlay{false};
  uint8_t bots{0};
  uint64_t seed{1};
  uint64_t playersSeed{kDefaultBotSeed};
};

// Parses argv[1..]. On failure returns false and sets `error`. Numbers accept
// decimal or 0x-prefixed hex.
[[nodiscard]] bool parseClientOptions(std::span<const char* const> args, ClientOptions& out,
                                      std::string& error);

}  // namespace kfm::client
