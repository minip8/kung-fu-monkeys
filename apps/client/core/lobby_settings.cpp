#include "lobby_settings.hpp"

#include "kfm/content/app_content.hpp"
#include "kfm/sim/constants.hpp"

#include <charconv>
#include <optional>
#include <system_error>

namespace kfm::client {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

[[nodiscard]] bool parseSmall(std::string_view v, uint32_t max, uint8_t& out) {
  uint32_t n = 0;
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
  if (ec != std::errc{} || end != v.data() + v.size() || v.empty() || n > max) return false;
  out = static_cast<uint8_t>(n);
  return true;
}

}  // namespace

bool lobbySettingsFrom(const ClientOptions& options, const sim::ContentDB& content,
                       LobbySettings& out, std::string& error) {
  sim::MatchConfig config{};
  if (!content::resolveMatchNames(content, options.arena, options.mode, config, error) ||
      !content::resolvePowerUpList(content, options.powerUps, config.powerUps, error)) {
    return false;
  }
  out.arena = config.arena;
  out.mode = config.mode;
  out.powerUps = config.powerUps;
  out.bots = options.bots;
  out.botTier = options.botDifficulty;
  out.volume = options.volume;
  out.mute = options.mute;
  return true;
}

void storeLobbySettings(const LobbySettings& settings, const sim::ContentDB& content,
                        ClientOptions& options) {
  options.arena = content.arenaAt(settings.arena).name.view();
  options.mode = content.modeAt(settings.mode).name.view();
  options.powerUps = content::powerUpListText(content, settings.powerUps);
  options.bots = settings.bots;
  options.botDifficulty = settings.botTier;
  options.volume = settings.volume;
  options.mute = settings.mute;
}

std::string settingsText(const ClientOptions& options) {
  std::string s;
  s += "arena=" + options.arena + "\n";
  s += "mode=" + options.mode + "\n";
  s += "powerups=" + (options.powerUps.empty() ? std::string("all") : options.powerUps) + "\n";
  s += "bots=" + std::to_string(options.bots) + "\n";
  s += "bot_difficulty=" + std::string(ai::toString(options.botDifficulty)) + "\n";
  s += "volume=" + std::to_string(options.volume) + "\n";
  s += std::string("mute=") + (options.mute ? "1" : "0") + "\n";
  return s;
}

void applySettingsText(std::string_view text, const sim::ContentDB& content, ClientOptions& options,
                       std::vector<std::string>& warnings) {
  int lineNo = 0;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    const std::string_view line = trim(text.substr(0, nl));
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    ++lineNo;
    if (line.empty() || line.front() == '#') continue;
    const auto warn = [&](const std::string& why) {
      warnings.push_back("settings line " + std::to_string(lineNo) + ": " + why);
    };
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      warn("expected key=value, got '" + std::string(line) + "'");
      continue;
    }
    const std::string_view key = trim(line.substr(0, eq));
    const std::string_view value = trim(line.substr(eq + 1));
    if (key == "arena" || key == "mode") {
      if (value.empty()) continue;  // the default
      const bool known = key == "arena" ? content.findArena(value).has_value()
                                        : content.findMode(value).has_value();
      if (!known) {
        warn("this content has no " + std::string(key) + " '" + std::string(value) + "'");
        continue;
      }
      (key == "arena" ? options.arena : options.mode) = value;
    } else if (key == "powerups") {
      uint16_t set = 0;
      std::string error;
      if (!content::resolvePowerUpList(content, value, set, error)) {
        warn(error);
        continue;
      }
      options.powerUps = value;
    } else if (key == "bots") {
      if (!parseSmall(value, sim::kMaxPlayers - 1u, options.bots)) warn("bots must be 0..9");
    } else if (key == "bot_difficulty") {
      const std::optional<ai::ReferenceTier> tier = ai::parseReferenceTier(value);
      if (tier) {
        options.botDifficulty = *tier;
      } else {
        warn("bot_difficulty must be easy, medium or hard");
      }
    } else if (key == "volume") {
      if (!parseSmall(value, 100, options.volume)) warn("volume must be 0..100");
    } else if (key == "mute") {
      uint8_t m = 0;
      if (parseSmall(value, 1, m)) {
        options.mute = m != 0;
      } else {
        warn("mute must be 0 or 1");
      }
    } else {
      warn("unknown key '" + std::string(key) + "'");
    }
  }
}

}  // namespace kfm::client
