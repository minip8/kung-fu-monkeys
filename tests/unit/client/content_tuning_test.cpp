#include "core/content_tuning.hpp"

#include "core/client_options.hpp"
#include "kfm/sim/content.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace kfm;
using namespace kfm::client;

TEST_CASE("tuning: per-second conversions round-trip the defaults", "[client][tuning]") {
  const sim::ContentDB& c = sim::defaultContent();
  const sim::AbilityDef& dash = c.abilityForSlot(sim::kAbilitySlotDash);
  CHECK(perTickToPerSecond(dash.speed) == Catch::Approx(22.0).margin(1e-3));
  CHECK(perTickToPerSecond(c.player.moveSpeed) == Catch::Approx(7.0).margin(1e-3));
  CHECK(perSecondToPerTick(16.0f) == c.boomerang.speed);
  CHECK(floatToFixed(1.0f) == Fixed::one());
  CHECK(fixedToFloat(Fixed::half()) == 0.5f);
}

TEST_CASE("tuning: Fixed values print as exact C++", "[client][tuning]") {
  CHECK(fixedToCpp(Fixed::one(), false) == "Fixed::one()");
  CHECK(fixedToCpp(Fixed::fromRatio(22, 60), true) == "perSecond(22)");
  CHECK(fixedToCpp(Fixed::fromRatio(35, 100), false) == "Fixed::fromRatio(35, 100)");
  CHECK(fixedToCpp(Fixed::fromRaw(12345), false) == "Fixed::fromRaw(12345)");
}

TEST_CASE("tuning: contentToCpp prints the tuned fields in content.cpp style", "[client][tuning]") {
  sim::ContentDB c = sim::defaultContent();
  const std::string def = contentToCpp(c);
  CHECK(def.find("  dash.cooldownTicks = 45;\n") != std::string::npos);
  CHECK(def.find("  dash.perfectWindowTicks = 4;\n") != std::string::npos);
  CHECK(def.find("  dash.speed = perSecond(22);\n") != std::string::npos);
  CHECK(def.find("  thr.cooldownTicks = 20;\n") != std::string::npos);
  CHECK(def.find("  c.boomerang.speed = perSecond(16);\n") != std::string::npos);
  CHECK(def.find("  c.boomerang.turnRateBam = 1200;\n") != std::string::npos);
  CHECK(def.find("  c.boomerang.restitution = Fixed::one();\n") != std::string::npos);
  CHECK(def.find("  c.player.moveSpeed = perSecond(7);\n") != std::string::npos);
  CHECK(def.find("  rounds.roundsToWin = 3;\n") != std::string::npos);

  c.abilities[static_cast<uint8_t>(sim::AbilityKind::Dash)].perfectWindowTicks = 6;
  c.player.moveSpeed = perSecondToPerTick(8.5f);
  const std::string tuned = contentToCpp(c);
  CHECK(tuned.find("  dash.perfectWindowTicks = 6;\n") != std::string::npos);
  CHECK(tuned.find("  c.player.moveSpeed = Fixed::fromRaw(") != std::string::npos);
}

TEST_CASE("options: flags parse", "[client][options]") {
  ClientOptions o;
  std::string err;
  const std::array<const char*, 12> args = {"kfm_client",
                                            "--autostart",
                                            "--bots",
                                            "3",
                                            "--seed",
                                            "0x2A",
                                            "--players-seed",
                                            "9",
                                            "--exit-after-frames",
                                            "600",
                                            "--screenshot",
                                            "a.ppm"};
  REQUIRE(parseClientOptions(args, o, err));
  CHECK(o.autostart);
  CHECK(o.bots == 3);
  CHECK(o.seed == 42);
  CHECK(o.playersSeed == 9);
  CHECK(o.exitAfterFrames == 600);
  CHECK(o.screenshotPath == "a.ppm");

  ClientOptions bad;
  const std::array<const char*, 3> tooMany = {"kfm_client", "--bots", "10"};
  CHECK_FALSE(parseClientOptions(tooMany, bad, err));
  const std::array<const char*, 2> unknown = {"kfm_client", "--nope"};
  CHECK_FALSE(parseClientOptions(unknown, bad, err));
  const std::array<const char*, 2> missing = {"kfm_client", "--seed"};
  CHECK_FALSE(parseClientOptions(missing, bad, err));
  const std::array<const char*, 3> zero = {"kfm_client", "--exit-after-frames", "0"};
  CHECK_FALSE(parseClientOptions(zero, bad, err));
}

TEST_CASE("client options: network flags", "[client][options]") {
  using kfm::client::ClientOptions;
  using kfm::client::parseClientOptions;
  std::string err;
  ClientOptions o;
  const std::array<const char*, 11> args = {
      "kfm_client", "--connect", "ws://127.0.0.1:9000",        "--token",   "abc",
      "--spectate", "--stun",    "stun:stun.example.org:3478", "--players", "2",
      "--autostart"};
  REQUIRE(parseClientOptions(args, o, err));
  CHECK(o.networkMode());
  CHECK(o.connect == "ws://127.0.0.1:9000");
  CHECK(o.token == "abc");
  CHECK(o.spectate);
  CHECK(o.stun == "stun:stun.example.org:3478");
  CHECK(o.players == 2);
  const kfm::client::NetSessionConfig c = kfm::client::netSessionConfig(o, true);
  CHECK(c.serverLabel == o.connect);
  CHECK(c.token.size() == 3);
  CHECK(c.spectate);
  CHECK(c.requestedSlots == 2);

  ClientOptions plain;
  const std::array<const char*, 3> connect = {"kfm_client", "--connect", "ws://h:1"};
  REQUIRE(parseClientOptions(connect, plain, err));
  CHECK(kfm::client::netSessionConfig(plain, true).requestedSlots == 1);   // at startup: one slot
  CHECK(kfm::client::netSessionConfig(plain, false).requestedSlots == 0);  // lobby: the devices
  CHECK_FALSE(ClientOptions{}.networkMode());

  ClientOptions bad;
  const std::array<const char*, 3> notWs = {"kfm_client", "--connect", "http://h:1"};
  CHECK_FALSE(parseClientOptions(notWs, bad, err));
  const std::array<const char*, 3> zeroPlayers = {"kfm_client", "--players", "0"};
  CHECK_FALSE(parseClientOptions(zeroPlayers, bad, err));
  const std::array<const char*, 2> noUrl = {"kfm_client", "--connect"};
  CHECK_FALSE(parseClientOptions(noUrl, bad, err));
}
