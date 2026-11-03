#include "debug_overlay.hpp"

#include "core/content_tuning.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/tags.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace kfm::client {

namespace {

[[nodiscard]] const char* phaseName(sim::MatchPhase p) {
  switch (p) {
    case sim::MatchPhase::Lobby:
      return "Lobby";
    case sim::MatchPhase::RoundStarting:
      return "RoundStarting";
    case sim::MatchPhase::Playing:
      return "Playing";
    case sim::MatchPhase::RoundEnded:
      return "RoundEnded";
    case sim::MatchPhase::MatchEnded:
      return "MatchEnded";
  }
  return "?";
}

[[nodiscard]] const char* abilityPhaseName(sim::AbilityPhase p) {
  switch (p) {
    case sim::AbilityPhase::Idle:
      return "Idle";
    case sim::AbilityPhase::Startup:
      return "Startup";
    case sim::AbilityPhase::Active:
      return "Active";
    case sim::AbilityPhase::Recovery:
      return "Recovery";
  }
  return "?";
}

[[nodiscard]] const char* abilityName(sim::AbilityKind k) {
  switch (k) {
    case sim::AbilityKind::Dash:
      return "dash";
    case sim::AbilityKind::Throw:
      return "throw";
    default:
      return "ability";
  }
}

[[nodiscard]] const char* bananaPhaseName(sim::BoomerangPhase p) {
  switch (p) {
    case sim::BoomerangPhase::Outbound:
      return "Outbound";
    case sim::BoomerangPhase::Returning:
      return "Returning";
    case sim::BoomerangPhase::Grounded:
      return "Grounded";
  }
  return "?";
}

[[nodiscard]] const char* ownerName(SlotOwner o) {
  switch (o) {
    case SlotOwner::None:
      return "-";
    case SlotOwner::Local:
      return "local";
    case SlotOwner::Dropped:
      return "dropped (repeat last)";
    case SlotOwner::Bot:
      return "bot";
  }
  return "?";
}

void tagsText(sim::Tags tags, char (&out)[96]) {
  constexpr std::array<const char*, static_cast<std::size_t>(sim::Tag::kCount)> kNames = {
      "Dashing", "PerfectDash", "HitStun", "Shielded", "Invulnerable", "Eliminated"};
  out[0] = '\0';
  std::size_t used = 0;
  for (std::size_t t = 0; t < kNames.size(); ++t) {
    if (!tags.has(static_cast<sim::Tag>(t))) continue;
    const int n =
        std::snprintf(out + used, sizeof out - used, "%s%s", used == 0 ? "" : " ", kNames[t]);
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof out - used) break;
    used += static_cast<std::size_t>(n);
  }
  if (used == 0) std::snprintf(out, sizeof out, "-");
}

bool sliderU16(const char* label, uint16_t& v, uint16_t lo, uint16_t hi) {
  return ImGui::SliderScalar(label, ImGuiDataType_U16, &v, &lo, &hi);
}

bool sliderU8(const char* label, uint8_t& v, uint8_t lo, uint8_t hi) {
  return ImGui::SliderScalar(label, ImGuiDataType_U8, &v, &lo, &hi);
}

bool sliderPerSecond(const char* label, Fixed& v, float lo, float hi) {
  float f = perTickToPerSecond(v);
  if (!ImGui::SliderFloat(label, &f, lo, hi, "%.2f u/s")) return false;
  v = perSecondToPerTick(f);
  return true;
}

bool sliderFixed(const char* label, Fixed& v, float lo, float hi) {
  float f = fixedToFloat(v);
  if (!ImGui::SliderFloat(label, &f, lo, hi, "%.3f")) return false;
  v = floatToFixed(f);
  return true;
}

void performanceSection(const MatchSession& s, const presentation::RendererStats& r,
                        const std::array<float, 240>& frameMs, std::size_t head) {
  const ImGuiIO& io = ImGui::GetIO();
  const FixedStepClock& clock = s.clock();
  ImGui::Text("frame %.2f ms  (%.1f fps)",
              static_cast<double>(io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f),
              static_cast<double>(io.Framerate));
  ImGui::PlotLines("frame ms", frameMs.data(), static_cast<int>(frameMs.size()),
                   static_cast<int>(head), nullptr, 0.0f, 50.0f, ImVec2(0, 50));
  const ClockStats& st = clock.stats();
  std::array<float, kTickHistogramBins> hist{};
  for (std::size_t i = 0; i < hist.size(); ++i)
    hist[i] = static_cast<float>(st.ticksPerFrame[i]);
  ImGui::PlotHistogram("ticks/frame", hist.data(), static_cast<int>(hist.size()), 0, nullptr, 0.0f,
                       FLT_MAX, ImVec2(0, 50));
  ImGui::TextDisabled("  bins: 0, 1, 2, ... %u+ ticks per frame", kTickHistogramBins - 1);
  ImGui::Text("ticks %llu over %llu frames, this frame %u (cap %u)",
              static_cast<unsigned long long>(st.ticks), static_cast<unsigned long long>(st.frames),
              s.ticksThisFrame(), clock.maxTicksPerFrame());
  ImGui::Text("backlog dropped: %llu ticks in %llu frames",
              static_cast<unsigned long long>(st.droppedTicks),
              static_cast<unsigned long long>(st.framesWithDrops));
  ImGui::Text("events this frame %zu, dropped %llu", s.frameEvents().size(),
              static_cast<unsigned long long>(s.droppedEvents()));
  ImGui::Text("renderer: %u draw calls, %u triangles", r.drawCalls, r.triangles);
}

void matchSection(const MatchSession& s) {
  const sim::WorldState& w = s.curr();
  ImGui::Text("session %s, sim phase %s (%u ticks in phase)",
              s.phase() == SessionPhase::Lobby ? "Lobby" : "Match", phaseName(w.rules.phase),
              w.rules.ticksInPhase);
  ImGui::Text("tick %u  checksum 0x%08X", w.tick.value, s.checksum());
  ImGui::Text("seed 0x%llX  bot seed 0x%llX  round %u", static_cast<unsigned long long>(s.seed()),
              static_cast<unsigned long long>(s.botSeed()), static_cast<unsigned>(w.rules.round));
  const bool tuned = !s.contentIsDefault();
  ImGui::Text("content hash 0x%08X", s.contentHash());
  ImGui::SameLine();
  if (tuned) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f), "(TUNED: differs from the default)");
  } else {
    ImGui::TextDisabled("(default)");
  }
}

void interpolationSection(MatchSession& s) {
  int policy = static_cast<int>(s.interpolationPolicy());
  constexpr const char* kPolicies[] = {"Auto", "Interpolate", "Newest tick"};
  if (ImGui::Combo("render policy", &policy, kPolicies, 3)) {
    s.setInterpolationPolicy(static_cast<InterpolationPolicy>(policy));
  }
  ImGui::Text("display %.1f Hz (measured), tick rate %.1f Hz", s.clock().displayHz(),
              static_cast<double>(sim::kTickHz) * s.clock().timeScale());
  ImGui::Text("%s, alpha %.3f (accumulator %.3f)",
              s.rendersNewest() ? "rendering the newest tick" : "interpolating",
              static_cast<double>(s.renderAlpha()), static_cast<double>(s.clock().alpha()));
}

void controlsSection(MatchSession& s, uint64_t nowNs) {
  bool paused = s.paused();
  if (ImGui::Checkbox("pause (P)", &paused)) s.setPaused(paused);
  ImGui::SameLine();
  if (ImGui::Button("step (O)")) {
    s.setPaused(true);
    s.requestSingleStep();
  }
  float scale = static_cast<float>(s.clock().timeScale());
  if (ImGui::SliderFloat("time scale", &scale, 0.25f, 2.0f, "%.2fx")) {
    s.setTimeScale(static_cast<double>(scale));
  }
  int cap = static_cast<int>(s.clock().maxTicksPerFrame());
  if (ImGui::SliderInt("max ticks/frame", &cap, 1, static_cast<int>(kDefaultMaxTicksPerFrame))) {
    s.clock().setMaxTicksPerFrame(static_cast<uint32_t>(cap));
  }
  if (ImGui::Button("restart (same seed)")) s.restart(nowNs, false);
  ImGui::SameLine();
  if (ImGui::Button("restart (new seed)")) s.restart(nowNs, true);
  if (s.phase() == SessionPhase::Match) {
    ImGui::SameLine();
    if (ImGui::Button("back to lobby")) s.returnToLobby();
  }
}

void entitySection(const MatchSession& s) {
  const sim::WorldState& w = s.curr();
  const sim::ContentDB& c = s.content();
  if (ImGui::TreeNode("players", "players (%u)", static_cast<unsigned>(w.players.size()))) {
    for (uint16_t i = 0; i < sim::kMaxPlayers; ++i) {
      if (!w.players.isAliveIndex(i)) continue;
      const sim::Player& p = w.players.items[i];
      char tags[96];
      tagsText(p.tags, tags);
      ImGui::Text("slot %u (%s)  pos (%.2f, %.2f)  hp %u/%u  ammo %u/%u", p.id.value,
                  ownerName(s.owner(p.id)), p.transform.pos.x.toDouble(),
                  p.transform.pos.y.toDouble(), p.hp, p.stats.maxHp, p.ammo, p.stats.ammoMax);
      ImGui::Text("    tags %s  hitstun %u", tags, p.hitStunTicks);
      for (uint8_t a = 0; a < sim::kAbilitySlots; ++a) {
        const sim::AbilityState& ab = p.abilities[a];
        ImGui::Text("    %s: %s %u ticks, cooldown %u", abilityName(c.abilityAt(ab.defIndex).kind),
                    abilityPhaseName(ab.phase), ab.phaseTicks, ab.cooldownTicks);
      }
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("bananas", "bananas (%u)", static_cast<unsigned>(w.boomerangs.size()))) {
    w.boomerangs.forEachAlive([&](EntityId id, const sim::Boomerang& b) {
      ImGui::Text("#%u.%u %s owner %u, %u ticks in phase, pos (%.2f, %.2f)", id.index,
                  id.generation, bananaPhaseName(b.phase), b.owner.value, b.ticksInPhase,
                  b.transform.pos.x.toDouble(), b.transform.pos.y.toDouble());
    });
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("pickups", "pickups (%u)", static_cast<unsigned>(w.pickups.size()))) {
    w.pickups.forEachAlive([&](EntityId id, const sim::Pickup& p) {
      ImGui::Text("#%u.%u banana at (%.2f, %.2f)", id.index, id.generation, p.pos.x.toDouble(),
                  p.pos.y.toDouble());
    });
    ImGui::TreePop();
  }
}

void tuningSection(MatchSession& s, std::string& printed) {
  sim::ContentDB& c = s.content();
  sim::AbilityDef& dash = c.abilities[c.player.abilities[sim::kAbilitySlotDash]];
  sim::AbilityDef& thr = c.abilities[c.player.abilities[sim::kAbilitySlotThrow]];
  ImGui::TextDisabled("Edits apply live (content is passed to step() every tick).");

  ImGui::SeparatorText("Dash");
  sliderU16("dash cooldown", dash.cooldownTicks, 0, 180);
  sliderU16("dash startup", dash.startupTicks, 0, 10);
  sliderU16("dash active", dash.activeTicks, 1, 60);
  sliderU16("dash recovery", dash.recoveryTicks, 0, 60);
  const auto maxWindow = static_cast<uint16_t>(std::max<int>(0, dash.activeTicks - 1));
  dash.perfectWindowTicks = std::min(dash.perfectWindowTicks, maxWindow);
  sliderU16("perfect window", dash.perfectWindowTicks, 0, maxWindow);
  sliderPerSecond("dash speed", dash.speed, 4.0f, 60.0f);
  if (dash.startupTicks != 0) {
    ImGui::TextColored(
        ImVec4(1, 0.6f, 0.1f, 1),
        "startup > 0 breaks content.cpp's static_assert (PerfectDash from the press tick)");
  }

  ImGui::SeparatorText("Throw");
  sliderU16("throw cooldown", thr.cooldownTicks, 0, 120);

  ImGui::SeparatorText("Banana");
  sliderPerSecond("banana speed", c.boomerang.speed, 4.0f, 40.0f);
  sliderU16("outbound ticks", c.boomerang.outboundTicks, 1, 120);
  sliderU16("max return ticks", c.boomerang.maxReturnTicks, 30, 600);
  sliderU16("turn rate (BAM/tick)", c.boomerang.turnRateBam, 0, 4096);
  sliderFixed("restitution", c.boomerang.restitution, 0.0f, 1.5f);

  ImGui::SeparatorText("Player");
  sliderPerSecond("move speed", c.player.moveSpeed, 2.0f, 20.0f);
  sliderU16("hit stun ticks", c.player.hitStunTicks, 0, 60);
  sliderU8("ammo max", c.player.ammoMax, 1, 5);
  c.player.ammoStart = std::min(c.player.ammoStart, c.player.ammoMax);

  ImGui::SeparatorText("Mode");
  sliderU8("rounds to win", c.mode.roundsToWin, 1, 9);
  sliderU16("round start ticks", c.mode.roundStartTicks, 0, 300);
  sliderU16("round end ticks", c.mode.roundEndTicks, 0, 600);

  ImGui::Spacing();
  if (ImGui::Button("reset to defaults")) s.resetContent();
  ImGui::SameLine();
  if (ImGui::Button("print as C++")) {
    printed = contentToCpp(s.content());
    std::printf("%s", printed.c_str());
    std::fflush(stdout);
  }
  if (!printed.empty()) {
    ImGui::InputTextMultiline("##cpp", printed.data(), printed.size() + 1, ImVec2(-1.0f, 160.0f),
                              ImGuiInputTextFlags_ReadOnly);
  }
}

}  // namespace

void DebugOverlay::recordFrame(double frameSeconds) {
  frameMs_[frameHead_] = static_cast<float>(frameSeconds * 1000.0);
  frameHead_ = (frameHead_ + 1) % frameMs_.size();
}

void DebugOverlay::draw(MatchSession& session, const presentation::RendererStats& renderer,
                        uint64_t nowNs) {
  if (!open) return;
  ImGui::SetNextWindowPos(ImVec2(12.0f, 70.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.0f, 560.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  if (ImGui::Begin("Debug (F1)", &open)) {
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
      performanceSection(session, renderer, frameMs_, frameHead_);
    }
    if (ImGui::CollapsingHeader("Match", ImGuiTreeNodeFlags_DefaultOpen)) matchSection(session);
    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
      controlsSection(session, nowNs);
    }
    if (ImGui::CollapsingHeader("Interpolation")) interpolationSection(session);
    if (ImGui::CollapsingHeader("Entities")) entitySection(session);
    if (ImGui::CollapsingHeader("Feel tuning")) tuningSection(session, printedCpp_);
  }
  ImGui::End();
}

}  // namespace kfm::client
