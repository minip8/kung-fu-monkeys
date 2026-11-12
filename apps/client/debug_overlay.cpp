#include "debug_overlay.hpp"

#include "core/content_tuning.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/content.hpp"
#include "kfm/sim/tags.hpp"
#include "ui_style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <optional>

namespace kfm::client {

namespace {

// ImGui's own font at its native size: dense numbers stay crisp.
constexpr float kDebugFontSize = 13.0f;

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
    case sim::BoomerangPhase::Sinking:  // v0.57
      return "Sinking";
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
    case SlotOwner::Remote:
      return "remote";
  }
  return "?";
}

void tagsText(sim::Tags tags, char (&out)[96]) {
  constexpr std::array<const char*, static_cast<std::size_t>(sim::Tag::kCount)> kNames = {
      "Dashing",  "PerfectDash", "HitStun",      "Shielded", "Invulnerable", "Eliminated",
      "Slamming", "Parrying",    "FriendlyFire", "Carrying", "Falling"};
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

template <class Session>
void performanceSection(const Session& s, const presentation::RendererStats& r,
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
    ImGui::TextColored(ui::vec4(ui::kWarning), "(TUNED: differs from the default)");
  } else {
    ImGui::TextDisabled("(default)");
  }
}

// Who the bots are and, per bot slot, the host's counters (AgentHost::stats)
// and, for a module, the runtime's (WasmAgent::stats).
void botsSection(const MatchSession& s) {
  const BotRoster& roster = s.botRoster();
  ImGui::Text("bots: %s%s", roster.describe().c_str(),
              roster.moduleActive() ? "  (wasm module)" : "  (reference bot)");
  if (!s.botSpecError().empty()) {
    ImGui::TextColored(ui::vec4(ui::kError), "module fell back: %s", s.botSpecError().c_str());
  }
  if (s.phase() != SessionPhase::Match || s.botMask() == 0) {
    ImGui::TextDisabled("no bot slots in play");
    return;
  }
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId slot{i};
    if (!s.agents().has(slot)) continue;
    const BotSlotStats st = s.botStats(slot);
    ImGui::Text("P%u  acts %u  substituted %u  paused %u%s", static_cast<unsigned>(i) + 1, st.acts,
                st.substitutions, st.paused, s.agents().paused(slot) ? "  (eliminated)" : "");
    if (st.module) {
      ImGui::SameLine();
      ImGui::Text(" traps %llu  over budget %llu  bad returns %llu  fuel %u (max %u)%s",
                  static_cast<unsigned long long>(st.traps),
                  static_cast<unsigned long long>(st.overBudget),
                  static_cast<unsigned long long>(st.badReturns), st.fuelLast, st.fuelMax,
                  st.alive ? "" : "  DEAD");
    }
  }
}

// v0.35: the calls of the match, newest last (slot, symbol, tick).
void callsSection(const MatchSession& s) {
  const std::span<const MatchSession::RecentCall> calls = s.recentCalls();
  if (calls.empty()) {
    ImGui::TextDisabled("no calls yet (a call ability shouts its aim byte)");
    return;
  }
  for (const MatchSession::RecentCall& c : calls) {
    ImGui::Text("tick %u  P%u called %3u (0x%02X)", c.tick, static_cast<unsigned>(c.slot) + 1,
                static_cast<unsigned>(c.symbol), static_cast<unsigned>(c.symbol));
  }
}

template <class Session>
void interpolationSection(Session& s) {
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

template <class Session>
void entitySection(const Session& s) {
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
      for (uint8_t m = 0; m < p.modifiers.count && m < sim::kMaxModifiers; ++m) {
        const sim::ActiveModifier& am = p.modifiers.items[m];
        ImGui::Text("    power-up %s: %u ticks left", c.powerUpAt(am.powerUp).name.cStr(),
                    am.ticksLeft);
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
      const char* what =
          p.kind == sim::PickupKind::PowerUp ? c.powerUpAt(p.powerUpIndex).name.cStr() : "banana";
      ImGui::Text("#%u.%u %s at (%.2f, %.2f)", id.index, id.generation, what, p.pos.x.toDouble(),
                  p.pos.y.toDouble());
    });
    ImGui::TreePop();
  }
}

void tuningSection(MatchSession& s, std::string& printed) {
  sim::ContentDB& c = s.editContent();  // live tuning: the one mutable path
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
        ui::vec4(ui::kWarning),
        "startup > 0 breaks content.cpp's static_assert (PerfectDash from the press tick)");
  }

  ImGui::SeparatorText("Throw");
  sliderU16("throw cooldown", thr.cooldownTicks, 0, 120);
  if (thr.speed > Fixed::zero()) {
    // v0.48, the charge: the multipliers of a tap and a full charge, and the
    // ticks between them (the rate per tick is derived, as the loader does).
    const int32_t span = (thr.param1 - thr.param0).raw;
    auto ticks = static_cast<uint16_t>(
        std::clamp<int32_t>((span + (thr.speed.raw / 2)) / thr.speed.raw, 1, 180));
    sliderFixed("charge min (tap)", thr.param0, 0.1f, 2.0f);
    sliderFixed("charge max", thr.param1, 0.2f, 3.0f);
    thr.param1 = std::max(thr.param1, thr.param0 + Fixed::fromRatio(1, 100));
    sliderU16("charge ticks", ticks, 1, 180);
    thr.speed = (thr.param1 - thr.param0) / int32_t{ticks};
  }

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
  sim::ModeDef& mode = c.modes[s.matchConfig().mode];
  ImGui::Text("%s", mode.name.cStr());
  const bool frags = sim::scoresPoints(mode.kind);
  if (!frags) sliderU8("rounds to win", mode.roundsToWin, 1, 9);
  sliderU16("round start ticks", mode.roundStartTicks, 0, 300);
  sliderU16("round end ticks", mode.roundEndTicks, 0, 600);
  if (frags) {
    sliderU8("score to win", mode.scoreToWin, 1, 50);
    sliderU16("respawn ticks", mode.respawnTicks, 1, 600);
    sliderU16("spawn protection ticks", mode.respawnInvulnTicks, 0, 600);
  }
  if (mode.kind == sim::ModeKind::Hill) sliderU16("hill ticks", mode.hillTicks, 1, 1200);
  if (mode.kind == sim::ModeKind::Control) {
    // v0.80: the zone-ticks per point reuse hill_ticks.
    sliderU16("zone ticks per point", mode.hillTicks, 1, 1200);
    sliderU16("capture ticks", mode.captureTicks, 1, 1200);
  }
  // v0.80: the mode's banana flight (0 = the banana's own, above).
  sliderU16("mode outbound ticks (0 = banana's)", mode.bananaOutboundTicks, 0, 120);
  sliderU16("mode turn rate (0 = banana's)", mode.bananaTurnRateBam, 0, 4096);
  if (mode.kind == sim::ModeKind::Capture) {
    sliderU16("flag return ticks", mode.flagReturnTicks, 1, 3600);
  }

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

[[nodiscard]] double ms(uint64_t ns) {
  return static_cast<double>(ns) * 1e-6;
}

void slotList(sim::PlayerMask m, char (&out)[64]) {
  out[0] = '\0';
  std::size_t used = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((m & (1u << i)) == 0) continue;
    const int n = std::snprintf(out + used, sizeof out - used, "%sP%u", used == 0 ? "" : " ",
                                static_cast<unsigned>(i) + 1U);
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof out - used) break;
    used += static_cast<std::size_t>(n);
  }
  if (used == 0) std::snprintf(out, sizeof out, "none");
}

void netSection(const NetSession& s, const std::array<float, 240>& rttMs,
                const std::array<float, 240>& depth, std::size_t head) {
  const net::NetClient& c = s.client();
  const net::ClientNetStats& st = c.netStats();
  const NetRates& r = s.rates();
  ImGui::Text("state %s", netPhaseName(s.phase()));
  ImGui::TextWrapped("%s", s.statusText().c_str());
  char slots[64];
  slotList(s.localSlots(), slots);
  ImGui::Text("local slots %s (requested %u, devices %u)%s", slots,
              static_cast<unsigned>(s.requestedSlots()), static_cast<unsigned>(s.deviceCount()),
              c.spectator() && s.running() ? "  SPECTATING" : "");
  const std::optional<Tick> server = c.estimatedServerTick(s.nowNs());
  const std::optional<Tick> confirmed = c.confirmedTick();
  ImGui::Text("local tick L %u, server tick ~%s%u, confirmed %s%u", c.localTick().value,
              server ? "" : "?", server ? server->value : 0U, confirmed ? "" : "?",
              confirmed ? confirmed->value : 0U);
  ImGui::TextDisabled("Pause, step and time scale are off online: the server's clock rules.");

  ImGui::SeparatorText("Link");
  ImGui::Text("RTT %.1f ms (min %.1f, max %.1f), jitter %.1f ms, loss %.2f %%", ms(st.rttNs),
              ms(st.minRttNs), ms(st.maxRttNs), ms(st.jitterNs),
              static_cast<double>(st.lossPpm) * 1e-4);
  ImGui::Text("lead %d ticks (target %d), arrival margin %d (smoothed %.2f)", st.leadTicks,
              st.targetLeadTicks, static_cast<int>(st.arrivalMargin),
              static_cast<double>(st.arrivalMarginQ8) / 256.0);
  ImGui::Text("clock rate %.2f %%, hard jumps %llu, dropped ticks %llu",
              static_cast<double>(st.ratePpm) * 1e-4, static_cast<unsigned long long>(st.hardJumps),
              static_cast<unsigned long long>(st.clockDroppedTicks));
  ImGui::PlotLines("RTT ms", rttMs.data(), static_cast<int>(rttMs.size()), static_cast<int>(head),
                   nullptr, 0.0f, FLT_MAX, ImVec2(0, 40));

  ImGui::SeparatorText("Rollback");
  ImGui::PlotLines("depth/frame", depth.data(), static_cast<int>(depth.size()),
                   static_cast<int>(head), nullptr, 0.0f, FLT_MAX, ImVec2(0, 40));
  // The histogram up to the deepest bucket seen (at least 16 buckets).
  std::array<float, net::kRollbackDepthBuckets> hist{};
  int buckets = 16;
  for (std::size_t d = 0; d < hist.size(); ++d) {
    hist[d] = static_cast<float>(st.rollbackDepth[d]);
    if (st.rollbackDepth[d] != 0) buckets = std::max(buckets, static_cast<int>(d) + 1);
  }
  ImGui::PlotHistogram("depth hist", hist.data(), buckets, 0, nullptr, 0.0f, FLT_MAX,
                       ImVec2(0, 50));
  ImGui::TextDisabled("  bins: rollback depth 0, 1, ... %d ticks (of %u; the last is >= %u)",
                      buckets - 1, net::kRollbackDepthBuckets, net::kRollbackDepthBuckets - 1);
  ImGui::Text("rollbacks %llu (%.1f/s), max depth %u, re-simulated %llu ticks",
              static_cast<unsigned long long>(st.rollbacks), r.rollbacksPerSec, st.maxRollbackDepth,
              static_cast<unsigned long long>(st.resimulatedTicks));
  ImGui::Text("mispredicted ticks %llu (%.1f/s): remote %llu, local %llu, roster %llu",
              static_cast<unsigned long long>(st.mispredictedTicks), r.mispredictedTicksPerSec,
              static_cast<unsigned long long>(st.mispredictedRemote),
              static_cast<unsigned long long>(st.mispredictedLocal),
              static_cast<unsigned long long>(st.mispredictedPresent));
  ImGui::Text("substitutions seen %llu (%.2f/s), past the ring %llu, overflows %llu",
              static_cast<unsigned long long>(st.substitutionsSeen), r.substitutionsPerSec,
              static_cast<unsigned long long>(st.anchorRollbacks),
              static_cast<unsigned long long>(st.rollbackOverflows));
  const net::EventJournalStats& j = c.journal().stats();
  ImGui::Text(
      "events: delivered %llu, deduplicated %llu, dropped %llu, retracted %llu",
      static_cast<unsigned long long>(j.delivered), static_cast<unsigned long long>(j.deduplicated),
      static_cast<unsigned long long>(j.dropped), static_cast<unsigned long long>(j.retracted));

  ImGui::SeparatorText("Consistency and traffic");
  ImGui::Text("checksums compared %llu, mismatched %llu, skipped %llu",
              static_cast<unsigned long long>(st.checksumsCompared),
              static_cast<unsigned long long>(st.checksumMismatches),
              static_cast<unsigned long long>(st.checksumsSkipped));
  ImGui::Text("resync requests %llu, snapshots loaded %llu",
              static_cast<unsigned long long>(st.resyncRequests),
              static_cast<unsigned long long>(st.snapshotsLoaded));
  ImGui::Text("up %.0f B/s, down %.0f B/s (total %llu / %llu B, %llu / %llu packets)",
              r.bytesUpPerSec, r.bytesDownPerSec, static_cast<unsigned long long>(st.bytesSent),
              static_cast<unsigned long long>(st.bytesReceived),
              static_cast<unsigned long long>(st.packetsSent),
              static_cast<unsigned long long>(st.packetsReceived));
  ImGui::Text("ticks %llu (%.1f/s), decode errors %llu", static_cast<unsigned long long>(st.ticks),
              r.ticksPerSec, static_cast<unsigned long long>(st.decodeErrors));
}

void netMatchSection(NetSession& s, const presentation::RendererStats& r) {
  const ImGuiIO& io = ImGui::GetIO();
  const sim::WorldState& w = s.curr();
  ImGui::Text("frame %.2f ms  (%.1f fps), %u ticks this frame, %zu events",
              static_cast<double>(io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f),
              static_cast<double>(io.Framerate), s.ticksThisFrame(), s.frameEvents().size());
  ImGui::Text("sim phase %s (%u ticks in phase), round %u", phaseName(w.rules.phase),
              w.rules.ticksInPhase, static_cast<unsigned>(w.rules.round));
  ImGui::Text("tick %u  checksum 0x%08X  content hash 0x%08X", w.tick.value, s.checksum(),
              sim::contentHash(s.content()));
  ImGui::Text("seed 0x%llX (from the server)",
              static_cast<unsigned long long>(s.client().matchConfig().seed));
  int policy = static_cast<int>(s.interpolationPolicy());
  constexpr const char* kPolicies[] = {"Auto", "Interpolate", "Newest tick"};
  if (ImGui::Combo("render policy", &policy, kPolicies, 3)) {
    s.setInterpolationPolicy(static_cast<InterpolationPolicy>(policy));
  }
  ImGui::Text("display %.1f Hz, %s, alpha %.3f", s.displayHz(),
              s.rendersNewest() ? "rendering the newest tick" : "interpolating",
              static_cast<double>(s.renderAlpha()));
  ImGui::Text("renderer: %u draw calls, %u triangles", r.drawCalls, r.triangles);
}

// Audio (§7): mute, master volume, device status and the mixer's voices.
// Works online too: sound is presentation, not the sim.
void audioSection(ClientAudio& a) {
  bool muted = a.muted();
  if (ImGui::Checkbox("mute", &muted)) a.setMuted(muted);
  int volume = a.volumePercent();
  if (ImGui::SliderInt("volume", &volume, 0, 100, "%d %%")) a.setVolumePercent(volume);
  if (ImGui::Button("test click")) a.click();
  ImGui::TextWrapped("device: %s", a.statusText().c_str());
  const presentation::Mixer& m = a.mixer();
  ImGui::Text("voices %d / %d, started %llu, stolen %llu", m.activeVoices(),
              presentation::kMixerVoices, static_cast<unsigned long long>(m.started()),
              static_cast<unsigned long long>(m.stolenVoices()));
  ImGui::Text("queued %zu frames, underruns %llu", a.device().queuedFrames(),
              static_cast<unsigned long long>(a.device().underruns()));
}

// M7 step 6: the replay viewer's session, read-only.
void replaySection(const ReplaySession& s) {
  const sim::ReplayHeader& h = s.header();
  const sim::ContentDB& c = s.content();
  const sim::MatchConfig& m = s.matchConfig();
  ImGui::Text("tick %u / %u%s  checksum 0x%08X", s.tick(), s.tickCount(), s.atEnd() ? " (end)" : "",
              s.checksum());
  ImGui::Text("sim phase %s (%u ticks in phase), round %u", phaseName(s.curr().rules.phase),
              s.curr().rules.ticksInPhase, static_cast<unsigned>(s.curr().rules.round));
  ImGui::Text("%s, speed %.2fx", s.paused() ? "paused" : "playing", s.speed());
  ImGui::Text("seed 0x%llX  players 0x%03X", static_cast<unsigned long long>(h.seed),
              static_cast<unsigned>(m.players));
  ImGui::Text("arena %s  mode %s  loadout %s", c.arenaAt(m.arena).name.cStr(),
              c.modeAt(m.mode).name.cStr(), c.loadoutAt(m.loadout).name.cStr());
  ImGui::Text("power-ups 0x%04X", static_cast<unsigned>(m.powerUps));
  ImGui::Text("content hash 0x%08X  build hash 0x%08X", h.contentHash, h.buildHash);
}

// The entity inspector's view of a replay: every slot is a recorded one.
struct ReplayEntities {
  const ReplaySession& replay;
  [[nodiscard]] const sim::WorldState& curr() const { return replay.curr(); }
  [[nodiscard]] const sim::ContentDB& content() const { return replay.content(); }
  [[nodiscard]] static SlotOwner owner(PlayerId /*slot*/) { return SlotOwner::Remote; }
};

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
  ImGui::PushFont(ui::debugFont(), kDebugFontSize);  // v0.43: the developer's font, not the game's
  if (ImGui::Begin("Debug (F1)", &open)) {
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
      performanceSection(session, renderer, frameMs_, frameHead_);
    }
    if (ImGui::CollapsingHeader("Match", ImGuiTreeNodeFlags_DefaultOpen)) matchSection(session);
    if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
      controlsSection(session, nowNs);
    }
    if (ImGui::CollapsingHeader("Bots")) botsSection(session);
    if (ImGui::CollapsingHeader("Calls")) callsSection(session);
    if (ImGui::CollapsingHeader("Interpolation")) interpolationSection(session);
    if (ImGui::CollapsingHeader("Entities")) entitySection(session);
    if (ImGui::CollapsingHeader("Feel tuning")) tuningSection(session, printedCpp_);
    if (audio != nullptr && ImGui::CollapsingHeader("Audio")) audioSection(*audio);
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace kfm::client

namespace kfm::client {

void DebugOverlay::draw(NetSession& session, const presentation::RendererStats& renderer) {
  rttMs_[netHead_] = static_cast<float>(ms(session.client().netStats().rttNs));
  depth_[netHead_] = static_cast<float>(session.client().frameReport().maxDepth);
  netHead_ = (netHead_ + 1) % rttMs_.size();
  if (!open) return;
  ImGui::SetNextWindowPos(ImVec2(12.0f, 70.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(470.0f, 640.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::PushFont(ui::debugFont(), kDebugFontSize);
  if (ImGui::Begin("Debug (F1)", &open)) {
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::PlotLines("frame ms", frameMs_.data(), static_cast<int>(frameMs_.size()),
                       static_cast<int>(frameHead_), nullptr, 0.0f, 50.0f, ImVec2(0, 50));
      netMatchSection(session, renderer);
    }
    if (ImGui::CollapsingHeader("Net", ImGuiTreeNodeFlags_DefaultOpen)) {
      netSection(session, rttMs_, depth_, netHead_);
    }
    if (ImGui::CollapsingHeader("Entities")) entitySection(session);
    if (audio != nullptr && ImGui::CollapsingHeader("Audio")) audioSection(*audio);
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace kfm::client

namespace kfm::client {

void DebugOverlay::draw(ReplaySession& session, const presentation::RendererStats& renderer) {
  if (!open) return;
  ImGui::SetNextWindowPos(ImVec2(12.0f, 70.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.0f, 520.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::PushFont(ui::debugFont(), kDebugFontSize);
  if (ImGui::Begin("Debug (F1)", &open)) {
    if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
      performanceSection(session, renderer, frameMs_, frameHead_);
    }
    if (ImGui::CollapsingHeader("Replay", ImGuiTreeNodeFlags_DefaultOpen)) replaySection(session);
    if (ImGui::CollapsingHeader("Interpolation")) interpolationSection(session);
    if (ImGui::CollapsingHeader("Entities")) entitySection(ReplayEntities{session});
    if (audio != nullptr && ImGui::CollapsingHeader("Audio")) audioSection(*audio);
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace kfm::client
