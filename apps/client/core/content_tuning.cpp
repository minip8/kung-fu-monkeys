#include "content_tuning.hpp"

#include "kfm/sim/constants.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace kfm::client {

namespace {

constexpr double kOne = static_cast<double>(Fixed::kOneRaw);
constexpr auto kTickHzInt = static_cast<int32_t>(sim::kTickHz);

[[nodiscard]] int32_t clampRaw(double raw) {
  if (std::isnan(raw)) return 0;
  if (raw >= static_cast<double>(INT32_MAX)) return INT32_MAX;
  if (raw <= static_cast<double>(INT32_MIN)) return INT32_MIN;
  return static_cast<int32_t>(std::llround(raw));
}

void appendLine(std::string& out, const char* lhs, const std::string& rhs) {
  out += "  ";
  out += lhs;
  out += " = ";
  out += rhs;
  out += ";\n";
}

void appendLine(std::string& out, const char* lhs, uint32_t value) {
  appendLine(out, lhs, std::to_string(value));
}

}  // namespace

float perTickToPerSecond(Fixed perTick) {
  return static_cast<float>(perTick.toDouble() * static_cast<double>(sim::kTickHz));
}

Fixed perSecondToPerTick(float perSecond) {
  return Fixed::fromRaw(
      clampRaw(static_cast<double>(perSecond) * kOne / static_cast<double>(sim::kTickHz)));
}

float fixedToFloat(Fixed v) {
  return v.toFloat();
}

Fixed floatToFixed(float v) {
  return Fixed::fromRaw(clampRaw(static_cast<double>(v) * kOne));
}

std::string fixedToCpp(Fixed v, bool perSecond) {
  if (v == Fixed::one()) return "Fixed::one()";
  if (v == Fixed::zero()) return "Fixed::zero()";
  if (perSecond) {
    const auto n = clampRaw(v.toDouble() * static_cast<double>(sim::kTickHz));
    if (Fixed::fromRatio(n, kTickHzInt) == v) return "perSecond(" + std::to_string(n) + ")";
  }
  const auto hundredths = clampRaw(v.toDouble() * 100.0);
  if (Fixed::fromRatio(hundredths, 100) == v) {
    return "Fixed::fromRatio(" + std::to_string(hundredths) + ", 100)";
  }
  return "Fixed::fromRaw(" + std::to_string(v.raw) + ")";
}

std::string contentToCpp(const sim::ContentDB& c) {
  std::string out;
  out += "  // Tuned in kfm_client (paste into makeDefaultContent(), engine/sim/src/content.cpp)\n";
  char hash[64];
  std::snprintf(hash, sizeof hash, "  // content hash 0x%08X\n", sim::contentHash(c));
  out += hash;

  const sim::AbilityDef& dash = c.abilityForSlot(sim::kAbilitySlotDash);
  appendLine(out, "dash.cooldownTicks", dash.cooldownTicks);
  appendLine(out, "dash.startupTicks", dash.startupTicks);
  appendLine(out, "dash.activeTicks", dash.activeTicks);
  appendLine(out, "dash.recoveryTicks", dash.recoveryTicks);
  appendLine(out, "dash.perfectWindowTicks", dash.perfectWindowTicks);
  appendLine(out, "dash.speed", fixedToCpp(dash.speed, true));

  const sim::AbilityDef& thr = c.abilityForSlot(sim::kAbilitySlotThrow);
  appendLine(out, "thr.cooldownTicks", thr.cooldownTicks);
  appendLine(out, "thr.startupTicks", thr.startupTicks);
  appendLine(out, "thr.activeTicks", thr.activeTicks);
  appendLine(out, "thr.recoveryTicks", thr.recoveryTicks);

  appendLine(out, "c.boomerang.speed", fixedToCpp(c.boomerang.speed, true));
  appendLine(out, "c.boomerang.outboundTicks", c.boomerang.outboundTicks);
  appendLine(out, "c.boomerang.maxReturnTicks", c.boomerang.maxReturnTicks);
  appendLine(out, "c.boomerang.turnRateBam", c.boomerang.turnRateBam);
  appendLine(out, "c.boomerang.restitution", fixedToCpp(c.boomerang.restitution, false));

  appendLine(out, "c.player.moveSpeed", fixedToCpp(c.player.moveSpeed, true));
  appendLine(out, "c.player.ammoMax", c.player.ammoMax);
  appendLine(out, "c.player.hitStunTicks", c.player.hitStunTicks);

  const sim::ModeDef& rounds = c.modes[0];
  appendLine(out, "rounds.roundsToWin", rounds.roundsToWin);
  appendLine(out, "rounds.roundEndTicks", rounds.roundEndTicks);
  appendLine(out, "rounds.roundStartTicks", rounds.roundStartTicks);
  return out;
}

}  // namespace kfm::client
