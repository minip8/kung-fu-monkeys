// Feel-tuning helpers for the debug UI (architecture §7, §12 M2 feel tuning):
// unit conversions between the sim's fixed-point per-tick values and the
// float per-second values the sliders show, and a printer that writes the
// tuned numbers in the initializer style of engine/sim/src/content.cpp so a
// result can be pasted back into the constexpr tables.
#pragma once

#include "kfm/core/fixed.hpp"
#include "kfm/sim/content.hpp"

#include <string>

namespace kfm::client {

// Units per tick (Fixed) <-> units per second (float), at kTickHz.
[[nodiscard]] float perTickToPerSecond(Fixed perTick);
[[nodiscard]] Fixed perSecondToPerTick(float perSecond);

[[nodiscard]] float fixedToFloat(Fixed v);
[[nodiscard]] Fixed floatToFixed(float v);

// C++ for a Fixed: "Fixed::one()", "perSecond(N)" when `perSecond` and the
// value is exactly content.cpp's perSecond(N), "Fixed::fromRatio(N, 100)" when
// exact, else "Fixed::fromRaw(R)" (always exact).
[[nodiscard]] std::string fixedToCpp(Fixed v, bool perSecond);

// The tunable fields of `c` as assignment lines in makeDefaultContent()'s style
// (dash.*, thr.*, c.boomerang.*, c.player.*, rounds.* = modes[0]), one per
// line. content/*.toml is the source of truth; these lines keep
// defaultContent() (the test fixture) in step with it.
[[nodiscard]] std::string contentToCpp(const sim::ContentDB& c);

}  // namespace kfm::client
