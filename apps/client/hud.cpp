#include "hud.hpp"

#include "kfm/content/app_content.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/event_fx.hpp"
#include "kfm/presentation/palette.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace kfm::client {

namespace {

using presentation::EffectKind;
using presentation::HudPhase;
using presentation::kNoSlot;

constexpr ImU32 kWhite = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
constexpr ImU32 kBananaYellow = IM_COL32(250, 214, 40, 255);
constexpr ImU32 kEmptyPip = IM_COL32(40, 40, 40, 200);

[[nodiscard]] ImU32 slotColor(uint32_t slot, int alpha = 255) {
  const presentation::Color c = presentation::playerColor(slot);
  const auto channel = [](float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return IM_COL32(channel(c.r), channel(c.g), channel(c.b), alpha);
}

[[nodiscard]] ImVec4 slotColorVec(uint32_t slot) {
  const presentation::Color c = presentation::playerColor(slot);
  return {c.r, c.g, c.b, 1.0f};
}

// v0.32: a team is drawn in the palette colour of its index (team 1 red,
// team 2 blue, ...), named "Team 1".. on screen.
[[nodiscard]] ImU32 teamColor(uint32_t team, int alpha = 255) {
  return slotColor(team, alpha);
}

// "P1".."P10" for local players, "BOT3" (slot number) for bots: several bots
// must stay distinguishable in the score bar and banners.
void slotLabel(const HudView& view, uint8_t slot, char (&out)[8]) {
  if (view.owners[slot] == SlotOwner::Bot) {
    std::snprintf(out, sizeof out, "BOT%u", static_cast<unsigned>(slot) + 1U);
  } else {
    std::snprintf(out, sizeof out, "P%u", static_cast<unsigned>(slot) + 1U);
  }
}

// Text with a drop shadow; `anchor` is the centre of the text box.
void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text) {
  ImFont* font = ImGui::GetFont();
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 pos{anchor.x - (extent.x * 0.5f), anchor.y - (extent.y * 0.5f)};
  const float off = std::max(1.0f, size * 0.06f);
  dl->AddText(font, size, ImVec2(pos.x + off, pos.y + off), kShadow, text);
  dl->AddText(font, size, pos, color, text);
}

[[nodiscard]] float textWidth(float size, const char* text) {
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

// "m:ss" of a clock, rounded up so it reads 0:00 only at the end.
void clockText(float seconds, char (&out)[16]) {
  const int total = std::max(0, static_cast<int>(std::ceil(seconds)));
  std::snprintf(out, sizeof out, "%d:%02d", total / 60, total % 60);
}

// The mode line under the score bar: round and target, the clock, how many
// are still in (survival). The clock pops on a TimerWarning and turns red in
// the last ten seconds.
void drawModeLine(ImDrawList* dl, const presentation::RenderState& state, float viewW, float y) {
  const presentation::RenderHud& hud = state.hud;
  char line[96];
  switch (hud.mode) {
    case sim::ModeKind::Deathmatch:
      std::snprintf(line, sizeof line, "Deathmatch  (first to %u)",
                    static_cast<unsigned>(hud.scoreToWin));
      break;
    case sim::ModeKind::TeamDeathmatch:
      std::snprintf(line, sizeof line, "Team deathmatch  (first team to %u)",
                    static_cast<unsigned>(hud.scoreToWin));
      break;
    case sim::ModeKind::Hill:
      std::snprintf(line, sizeof line, "Hill  (first team to %u points)",
                    static_cast<unsigned>(hud.scoreToWin));
      break;
    case sim::ModeKind::Capture:
      std::snprintf(line, sizeof line, "Capture  (first team to %u flags)",
                    static_cast<unsigned>(hud.scoreToWin));
      break;
    case sim::ModeKind::TeamRounds:
      std::snprintf(line, sizeof line, "Team round %u  (first team to %u)",
                    static_cast<unsigned>(hud.round), static_cast<unsigned>(hud.roundsToWin));
      break;
    case sim::ModeKind::Survival:
      std::snprintf(line, sizeof line, "Round %u  (first to %u)  %u alive",
                    static_cast<unsigned>(hud.round), static_cast<unsigned>(hud.roundsToWin),
                    static_cast<unsigned>(hud.aliveCount));
      break;
    case sim::ModeKind::Rounds:
    case sim::ModeKind::kCount:
      std::snprintf(line, sizeof line, "Round %u  (first to %u)", static_cast<unsigned>(hud.round),
                    static_cast<unsigned>(hud.roundsToWin));
      break;
  }
  centredText(dl, 16.0f, ImVec2(viewW * 0.5f, y), kWhite, line);
  if (hud.teamCount > 0) {
    // v0.32: the team scores, each in its team's colour, side by side.
    y += 22.0f;
    constexpr float kTeamW = 128.0f;
    const uint8_t teams = std::min<uint8_t>(hud.teamCount, sim::kMaxPlayers);
    float x = (viewW * 0.5f) - (static_cast<float>(teams - 1) * kTeamW * 0.5f);
    for (uint8_t t = 0; t < teams; ++t) {
      char text[32];
      std::snprintf(text, sizeof text, "Team %u: %u", static_cast<unsigned>(t) + 1U,
                    static_cast<unsigned>(hud.teamScores[t]));
      centredText(dl, 18.0f, ImVec2(x, y), teamColor(t), text);
      x += kTeamW;
    }
  }
  if (!hud.hasClock) return;
  char clock[16];
  clockText(hud.clockSeconds, clock);
  const bool urgent = hud.phase == HudPhase::Playing && hud.clockSeconds <= 10.0f;
  const float pop = presentation::bannerScale(state, EffectKind::TimerBanner);
  centredText(dl, 26.0f * pop, ImVec2(viewW * 0.5f, y + 24.0f),
              urgent ? IM_COL32(255, 90, 70, 255) : kWhite, clock);
}

// The one power-up timer widget (HUD cards and the over-head row): a pie in
// the power-up's colour, filled clockwise from 12 o'clock with the time left.
void drawTimerPie(ImDrawList* dl, ImVec2 c, float r, uint8_t colorIndex, float fraction) {
  dl->AddCircleFilled(c, r, kEmptyPip);
  if (fraction > 0.0f) {
    const float start = -0.5f * presentation::kPi;
    dl->PathLineTo(c);
    dl->PathArcTo(c, r, start, start + (2.0f * presentation::kPi * fraction), 20);
    dl->PathFillConvex(slotColor(colorIndex));
  }
  dl->AddCircle(c, r, kShadow, 0, 1.0f);
}

// The card every slot is drawn on, in the lobby and in the HUD: a dark
// rounded box with the slot's palette colour as a stripe along the top.
constexpr float kCardStripe = 4.0f;
constexpr float kCardRounding = 6.0f;
void drawCardFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, uint8_t slot, int alpha) {
  dl->AddRectFilled(min, max, IM_COL32(0, 0, 0, (130 * alpha) / 255), kCardRounding);
  dl->AddRectFilled(min, ImVec2(max.x, min.y + kCardStripe), slotColor(slot, alpha), kCardRounding,
                    ImDrawFlags_RoundCornersTop);
}

[[nodiscard]] const presentation::RenderPlayer* playerOfSlot(const presentation::RenderState& rs,
                                                             uint8_t slot) {
  for (uint16_t k = 0; k < rs.playerCount; ++k) {
    if (rs.players[k].slot == slot) return &rs.players[k];
  }
  return nullptr;
}

// The scoreboard (v0.24): one card per present slot along the top, the same
// layout in every mode. The top line is the slot's label and its score
// (round-win pips in rounds and survival, eliminations in deathmatch, with
// the respawn countdown in brackets while it waits); the line under it is
// the slot's live power-ups (§5.8), one timer pie each with its whole
// seconds. Cards share one width so the row does not jitter, and the row
// shrinks to fit narrow windows.
void drawScoreCards(ImDrawList* dl, const HudView& view, const presentation::RenderState& state,
                    float viewW) {
  const presentation::RenderHud& hud = state.hud;
  const bool frags = sim::scoresPoints(hud.mode);
  int entries = 0;
  uint8_t timers = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    ++entries;
    if (const presentation::RenderPlayer* p = playerOfSlot(state, i)) {
      timers = std::max(timers, p->powerUpCount);
    }
  }
  if (entries == 0) return;

  constexpr float kLabel = 18.0f;
  constexpr float kPipR = 5.0f;
  constexpr float kPad = 8.0f;
  constexpr float kGap = 8.0f;
  constexpr float kTimerStep = 30.0f;
  constexpr float kPieR = 6.0f;
  const float labelW = textWidth(kLabel, "BOT10");
  const float scoreW = frags ? textWidth(kLabel, "00 (0)")
                             : static_cast<float>(hud.roundsToWin) * ((kPipR * 2.0f) + 4.0f);
  const float baseW = std::max((kPad * 2.0f) + labelW + 8.0f + scoreW,
                               (kPad * 2.0f) + (static_cast<float>(timers) * kTimerStep));
  const float baseH = kCardStripe + 4.0f + kLabel + 6.0f + (timers > 0 ? 18.0f : 0.0f);
  const float total =
      (static_cast<float>(entries) * baseW) + (static_cast<float>(entries - 1) * kGap);
  const float k = std::min(1.0f, (viewW - 24.0f) / total);  // shrink to fit
  const float w = baseW * k;
  const float h = baseH * k;
  float x = (viewW - (total * k)) * 0.5f;
  const float y = 10.0f;
  ImFont* font = ImGui::GetFont();
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    const bool waiting = hud.respawnSeconds[i] > 0.0f;
    // Dimmed while out of play: waiting to respawn, or eliminated this round.
    const bool out =
        waiting || (hud.phase == HudPhase::Playing && playerOfSlot(state, i) == nullptr);
    const int alpha = out ? 120 : 255;
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + h), i, alpha);
    if (hud.teamCount > 0) {
      // v0.32: the slot's team as a bar in the team colour along the bottom.
      dl->AddRectFilled(ImVec2(x, y + h - (3.0f * k)), ImVec2(x + w, y + h),
                        teamColor(hud.teams[i], alpha), kCardRounding,
                        ImDrawFlags_RoundCornersBottom);
    }
    const float lineY = y + ((kCardStripe + 4.0f) * k);
    char label[8];
    slotLabel(view, i, label);
    dl->AddText(font, kLabel * k, ImVec2(x + (kPad * k) + 1.0f, lineY + 1.0f), kShadow, label);
    dl->AddText(font, kLabel * k, ImVec2(x + (kPad * k), lineY), slotColor(i, alpha), label);
    const float right = x + w - (kPad * k);
    if (frags) {
      char score[16];
      if (waiting) {
        std::snprintf(score, sizeof score, "%u (%d)", static_cast<unsigned>(hud.scores[i]),
                      static_cast<int>(std::ceil(hud.respawnSeconds[i])));
      } else {
        std::snprintf(score, sizeof score, "%u", static_cast<unsigned>(hud.scores[i]));
      }
      const float sw = textWidth(kLabel * k, score);
      dl->AddText(font, kLabel * k, ImVec2(right - sw, lineY), kWhite, score);
    } else {
      // Right-aligned pips, the won ones first.
      const float r = kPipR * k;
      const float step = (r * 2.0f) + (4.0f * k);
      float px = right - r - (static_cast<float>(hud.roundsToWin) - 1.0f) * step;
      for (uint32_t pip = 0; pip < hud.roundsToWin; ++pip) {
        const ImVec2 c{px, lineY + (kLabel * k * 0.5f)};
        if (pip < hud.scores[i]) {
          dl->AddCircleFilled(c, r, slotColor(i));
        } else {
          dl->AddCircleFilled(c, r, kEmptyPip);
          dl->AddCircle(c, r, slotColor(i), 0, 1.5f);
        }
        px += step;
      }
    }
    if (const presentation::RenderPlayer* p = playerOfSlot(state, i)) {
      const float ty = lineY + ((kLabel + 6.0f + 8.0f) * k);
      float tx = x + ((kPad + kPieR) * k);
      for (uint8_t t = 0; t < p->powerUpCount; ++t) {
        const presentation::RenderPowerUp& pu = p->powerUps[t];
        drawTimerPie(dl, ImVec2(tx, ty), kPieR * k, pu.colorIndex, pu.fraction);
        char secs[8];
        std::snprintf(secs, sizeof secs, "%d", static_cast<int>(std::ceil(pu.secondsLeft)));
        dl->AddText(font, 12.0f * k, ImVec2(tx + ((kPieR + 3.0f) * k), ty - (6.0f * k)), kWhite,
                    secs);
        tx += kTimerStep * k;
      }
    }
    x += w + (kGap * k);
  }
  drawModeLine(dl, state, viewW, y + h + 14.0f);
}

// The banner title of a round or match winner: "Team 2" in a team mode, the
// slot's label otherwise, with the colour to draw it in.
void winnerText(const HudView& session, const presentation::RenderHud& hud, uint8_t winner,
                char (&out)[16], ImU32& color) {
  if (hud.teamCount > 0 && winner < sim::kMaxPlayers) {
    std::snprintf(out, sizeof out, "Team %u", static_cast<unsigned>(hud.teams[winner]) + 1U);
    color = teamColor(hud.teams[winner]);
    return;
  }
  char label[8];
  slotLabel(session, winner, label);
  std::snprintf(out, sizeof out, "%s", label);
  color = slotColor(winner);
}

// Over each monkey a compact row of the same timer pies (no seconds: the
// card has them), under the ammo pips.
void drawPowerUpDots(ImDrawList* dl, const presentation::RenderPlayer& p, ImVec2 centre) {
  if (p.powerUpCount == 0) return;
  constexpr float kPieR = 4.5f;
  constexpr float kStep = 12.0f;
  float x = centre.x - (static_cast<float>(p.powerUpCount - 1) * kStep * 0.5f);
  for (uint8_t i = 0; i < p.powerUpCount; ++i) {
    const presentation::RenderPowerUp& pu = p.powerUps[i];
    drawTimerPie(dl, ImVec2(x, centre.y), kPieR, pu.colorIndex, pu.fraction);
    x += kStep;
  }
}

void drawPlayerTags(ImDrawList* dl, const HudView& session, const HudInput& in) {
  const presentation::RenderState& rs = *in.state;
  for (uint16_t k = 0; k < rs.playerCount; ++k) {
    const presentation::RenderPlayer& p = rs.players[k];
    if (!p.visible) continue;
    const presentation::Vec3 head =
        p.position + presentation::Vec3{0.0f, rs.playerHeight + 0.45f, 0.0f};
    const presentation::ScreenPoint sp = presentation::worldToScreen(*in.camera, head);
    if (!sp.inFront) continue;
    const ImVec2 at{sp.x * in.uiScale, sp.y * in.uiScale};

    char label[8];
    slotLabel(session, p.slot, label);
    centredText(dl, 15.0f, ImVec2(at.x, at.y - 16.0f), slotColor(p.slot), label);

    constexpr float kPipR = 4.0f;
    const float width = static_cast<float>(p.ammoMax) * (kPipR * 2.0f + 3.0f) - 3.0f;
    float x = at.x - (width * 0.5f) + kPipR;
    for (uint8_t a = 0; a < p.ammoMax; ++a) {
      const ImVec2 c{x, at.y};
      if (a < p.ammo) {
        dl->AddCircleFilled(c, kPipR, kBananaYellow);
      } else {
        dl->AddCircleFilled(c, kPipR, kEmptyPip);
      }
      dl->AddCircle(c, kPipR, kShadow, 0, 1.0f);
      x += (kPipR * 2.0f) + 3.0f;
    }
    drawPowerUpDots(dl, p, ImVec2(at.x, at.y + 12.0f));
  }
}

// Banners pop in with their effect (bannerScale, event_fx.hpp).
void drawBanners(ImDrawList* dl, const HudView& session, const presentation::RenderState& state,
                 ImVec2 view) {
  const presentation::RenderHud& hud = state.hud;
  const ImVec2 centre{view.x * 0.5f, view.y * 0.42f};
  char text[64];
  switch (hud.phase) {
    case HudPhase::RoundStarting: {
      const float pop = presentation::bannerScale(state, EffectKind::RoundStartBanner);
      if (hud.mode == sim::ModeKind::Deathmatch) {
        std::snprintf(text, sizeof text, "Deathmatch");
      } else if (hud.mode == sim::ModeKind::TeamDeathmatch) {
        std::snprintf(text, sizeof text, "Team deathmatch");
      } else if (hud.mode == sim::ModeKind::Hill) {
        std::snprintf(text, sizeof text, "King of the hill");
      } else if (hud.mode == sim::ModeKind::Capture) {
        std::snprintf(text, sizeof text, "Capture the flag");
      } else {
        std::snprintf(text, sizeof text, "Round %u", static_cast<unsigned>(hud.round));
      }
      centredText(dl, 40.0f * pop, ImVec2(centre.x, centre.y - 50.0f), kWhite, text);
      const int n = std::max(1, static_cast<int>(std::ceil(hud.countdownSeconds)));
      std::snprintf(text, sizeof text, "%d", n);
      centredText(dl, 96.0f, ImVec2(centre.x, centre.y + 20.0f), kBananaYellow, text);
      break;
    }
    case HudPhase::RoundEnded: {
      const float pop = presentation::bannerScale(state, EffectKind::RoundEndBanner);
      if (hud.lastRoundWinner == kNoSlot) {
        centredText(dl, 56.0f * pop, centre, kWhite, "Draw!");
      } else {
        const auto w = static_cast<uint8_t>(hud.lastRoundWinner);
        char label[16];
        ImU32 color = kWhite;
        winnerText(session, hud, w, label, color);
        std::snprintf(text, sizeof text, "Round won by %s", label);
        centredText(dl, 56.0f * pop, centre, color, text);
      }
      break;
    }
    case HudPhase::MatchEnded: {
      if (hud.matchWinner != kNoSlot) {
        const auto w = static_cast<uint8_t>(hud.matchWinner);
        char label[16];
        ImU32 color = kWhite;
        winnerText(session, hud, w, label, color);
        std::snprintf(text, sizeof text, "%s wins the match!", label);
        centredText(dl, 64.0f * presentation::bannerScale(state, EffectKind::MatchEndBanner),
                    centre, color, text);
      } else {
        // A tied lead when a mode's clock ran out (deathmatch).
        centredText(dl, 64.0f * presentation::bannerScale(state, EffectKind::MatchEndBanner),
                    centre, kWhite, "Draw!");
      }
      centredText(dl, 20.0f, ImVec2(centre.x, centre.y + 60.0f), kWhite,
                  session.network ? "Esc: leave the server" : "Enter / Start: back to the lobby");
      if (!session.nextMatch.empty()) {
        centredText(dl, 18.0f, ImVec2(centre.x, centre.y + 88.0f), IM_COL32(200, 220, 255, 230),
                    session.nextMatch.c_str());
      }
      break;
    }
    case HudPhase::Lobby:
    case HudPhase::Playing:
      break;
  }
}

void drawNotices(ImDrawList* dl, const HudView& view, ImVec2 view2) {
  float y = view2.y - 28.0f;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((view.dropped & (1u << i)) == 0) continue;
    char text[128];
    std::snprintf(text, sizeof text,
                  "P%u's controller disconnected: repeating its last input. Press a button on any "
                  "free device to take over.",
                  static_cast<unsigned>(i) + 1U);
    centredText(dl, 18.0f, ImVec2(view2.x * 0.5f, y), slotColor(i), text);
    y -= 24.0f;
  }
  if (view.paused) {
    centredText(dl, 28.0f, ImVec2(view2.x * 0.5f, view2.y * 0.2f), kWhite,
                "PAUSED  (P: resume, O: step)");
  }
  if (!view.statusLine.empty()) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 pos{12.0f, view2.y - 22.0f};
    dl->AddText(font, 15.0f, ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadow, view.statusLine.c_str());
    dl->AddText(font, 15.0f, pos, IM_COL32(200, 220, 255, 230), view.statusLine.c_str());
  }
}

// Short device names for the lobby cards.
void shortDeviceText(const platform::DeviceBinding& d, char (&out)[24]) {
  switch (d.kind) {
    case platform::DeviceKind::Keyboard:
      if (d.keyboardProfile == 0) {
        std::snprintf(out, sizeof out, "keys left");
      } else if (d.keyboardProfile == 1) {
        std::snprintf(out, sizeof out, "keys right");
      } else {
        std::snprintf(out, sizeof out, "keys %u", static_cast<unsigned>(d.keyboardProfile));
      }
      return;
    case platform::DeviceKind::Gamepad:
      std::snprintf(out, sizeof out, "pad #%u", static_cast<unsigned>(d.padId));
      return;
    case platform::DeviceKind::None:
      break;
  }
  std::snprintf(out, sizeof out, "-");
}

[[nodiscard]] ImU32 toU32(presentation::Color c, int alpha = 255) {
  const auto channel = [](float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return IM_COL32(channel(c.r), channel(c.g), channel(c.b), alpha);
}

// One lobby card per slot (v0.24), in the HUD's card style: the slot's
// palette colour on top, its label and who drives it; free slots are dimmed.
// Ten in a row, or two rows of five when the window is narrow.
struct LobbyCard {
  bool filled{false};
  char title[8]{};
  char sub[24]{};
  int team{-1};  // v0.32: the slot's team in a team mode, -1 otherwise
};
void drawLobbyCards(const std::array<LobbyCard, sim::kMaxPlayers>& cards) {
  constexpr float kGap = 6.0f;
  constexpr float kH = 44.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const int perRow = avail >= (10.0f * 64.0f) + (9.0f * kGap) ? 10 : 5;
  const float w = (avail - (static_cast<float>(perRow - 1) * kGap)) / static_cast<float>(perRow);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const int rows = (static_cast<int>(sim::kMaxPlayers) + perRow - 1) / perRow;
  ImGui::Dummy(ImVec2(avail, (static_cast<float>(rows) * (kH + kGap)) - kGap));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const LobbyCard& c = cards[i];
    const int column = i % perRow;
    const int row = i / perRow;
    const float x = origin.x + (static_cast<float>(column) * (w + kGap));
    const float y = origin.y + (static_cast<float>(row) * (kH + kGap));
    const int alpha = c.filled ? 255 : 70;
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + kH), i, alpha);
    const ImU32 titleColor = c.filled ? slotColor(i) : IM_COL32(150, 150, 150, 160);
    dl->AddText(font, 17.0f, ImVec2(x + 7.0f, y + kCardStripe + 3.0f), titleColor, c.title);
    dl->AddText(font, 13.0f, ImVec2(x + 7.0f, y + kCardStripe + 22.0f),
                c.filled ? IM_COL32(220, 220, 220, 255) : IM_COL32(150, 150, 150, 140), c.sub);
    if (c.team >= 0) {
      // v0.32: "T1".. at the right of the title and a bar in the team colour.
      const auto team = static_cast<uint32_t>(c.team);
      char tag[16];
      std::snprintf(tag, sizeof tag, "T%u", team + 1U);
      const float tw = textWidth(15.0f, tag);
      dl->AddText(font, 15.0f, ImVec2(x + w - tw - 6.0f, y + kCardStripe + 4.0f),
                  teamColor(team, alpha), tag);
      dl->AddRectFilled(ImVec2(x, y + kH - 3.0f), ImVec2(x + w, y + kH), teamColor(team, alpha),
                        kCardRounding, ImDrawFlags_RoundCornersBottom);
    }
  }
}

// A top-down sketch of an arena from its def: floor, walls, pits (with their
// rim), slow patches, spawn points in the slot colours (spawn order) and the
// power-up spawners, filled when the match's power-up set leaves them
// something to spawn and hollow when it switches them off.
void drawArenaPreview(const sim::ContentDB& db, const sim::ArenaDef& arena, uint16_t powerUps,
                      ImVec2 box) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Dummy(box);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float hx = std::max(arena.halfExtents.x.toFloat(), 0.01f);
  const float hy = std::max(arena.halfExtents.y.toFloat(), 0.01f);
  const float scale = std::min((box.x - 8.0f) / (2.0f * hx), (box.y - 8.0f) / (2.0f * hy));
  const ImVec2 c{origin.x + (box.x * 0.5f), origin.y + (box.y * 0.5f)};
  const auto at = [&](float x, float y) { return ImVec2(c.x + (x * scale), c.y - (y * scale)); };
  const auto box2 = [&](Vec2 min, Vec2 max) {
    return std::pair{at(min.x.toFloat(), max.y.toFloat()), at(max.x.toFloat(), min.y.toFloat())};
  };
  dl->AddRectFilled(at(-hx, hy), at(hx, -hy), IM_COL32(72, 64, 54, 255));
  for (uint8_t i = 0; i < arena.hazardCount; ++i) {
    const sim::Hazard& hz = arena.hazards[i];
    const auto [a, b] = box2(hz.min, hz.max);
    if (hz.kind == sim::HazardKind::Pit) {
      dl->AddRectFilled(a, b, toU32(presentation::kArenaPitColor));
      dl->AddRect(a, b, toU32(presentation::kArenaPitRimColor), 0.0f, 0, 2.0f);
    } else {
      dl->AddRectFilled(a, b, toU32(presentation::kArenaSlowColor));
    }
  }
  for (uint8_t i = 0; i < arena.wallCount; ++i) {
    const auto [a, b] = box2(arena.walls[i].min, arena.walls[i].max);
    dl->AddRectFilled(a, b, toU32(presentation::kArenaWallColor));
  }
  dl->AddRect(at(-hx, hy), at(hx, -hy), toU32(presentation::kArenaWallColor), 0.0f, 0, 3.0f);
  for (uint8_t i = 0; i < arena.spawnPointCount; ++i) {
    const Vec2 p = arena.spawnPoints[i];
    dl->AddCircleFilled(at(p.x.toFloat(), p.y.toFloat()), 3.5f, slotColor(i));
  }
  const uint32_t all = (uint32_t{1} << std::min<uint8_t>(db.powerUpCount, sim::kMaxPowerUps)) - 1u;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount; ++i) {
    const sim::PickupSpawner& sp = arena.pickupSpawners[i];
    const uint32_t own = sp.powerUpSet == 0 ? all : (sp.powerUpSet & all);
    const bool live = (own & powerUps) != 0;
    const ImVec2 q = at(sp.pos.x.toFloat(), sp.pos.y.toFloat());
    constexpr float kR = 5.0f;
    const ImVec2 top{q.x, q.y - kR};
    const ImVec2 right{q.x + kR, q.y};
    const ImVec2 bottom{q.x, q.y + kR};
    const ImVec2 left{q.x - kR, q.y};
    if (live) {
      dl->AddQuadFilled(top, right, bottom, left, IM_COL32(255, 255, 255, 230));
    } else {
      dl->AddQuad(top, right, bottom, left, IM_COL32(170, 170, 170, 200), 1.5f);
    }
  }
}

// The match a config selects, as text: arena, mode with its key numbers,
// the loadout with its abilities, the power-up set. Shared by the online
// lobby and the local preview column.
void matchConfigText(const sim::ContentDB& db, const sim::MatchConfig& config) {
  const sim::ArenaDef& arena = db.arenaAt(config.arena);
  const sim::ModeDef& mode = db.modeAt(config.mode);
  ImGui::Text("Arena  %s", arena.name.cStr());
  ImGui::Text("Mode   %s", mode.name.cStr());
  if (mode.teams > 0) {
    ImGui::Text("Teams  %u, balanced as players join", static_cast<unsigned>(mode.teams));
  }
  ImGui::TextWrapped("%s", modeSummary(mode).c_str());
  ImGui::Text("Kit    %s (%s)", db.loadoutAt(config.loadout).name.cStr(),
              loadoutSummary(db, config.loadout).c_str());
  if (arena.pickupSpawnerCount == 0) {
    ImGui::TextDisabled("No power-up spawners in this arena.");
  } else {
    ImGui::TextWrapped("Power-ups: %s", content::powerUpListText(db, config.powerUps).c_str());
  }
}

}  // namespace

HudView hudView(const MatchSession& session) {
  HudView v;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    v.owners[i] = session.owner(PlayerId{i});
  v.dropped = static_cast<sim::PlayerMask>(session.droppedMask() & session.localMask());
  v.paused = session.paused();
  return v;
}

HudView hudView(const NetSession& session) {
  HudView v;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    v.owners[i] = session.owner(PlayerId{i});
  v.dropped = session.droppedMask();
  v.network = true;
  v.statusLine = session.statusText();
  if (session.configKnown()) {
    const sim::MatchConfig& next = session.client().matchConfig();
    const sim::ContentDB& db = session.content();
    v.nextMatch = std::string("Next match: ") + db.arenaAt(next.arena).name.cStr() + ", " +
                  db.modeAt(next.mode).name.cStr() + ", loadout " +
                  db.loadoutAt(next.loadout).name.cStr() +
                  ", power-ups: " + content::powerUpListText(db, next.powerUps);
  }
  return v;
}

void drawMatchHud(const HudView& view, const HudInput& in) {
  if (in.state == nullptr || in.camera == nullptr) return;
  // The background list draws over the 3D scene (rendered before ImGui) but under
  // ImGui windows, so the debug overlay stays readable.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  drawPlayerTags(dl, view, in);
  drawScoreCards(dl, view, *in.state, display.x);
  drawBanners(dl, view, *in.state, display);
  drawNotices(dl, view, display);
}

void drawNetStatus(const NetSession& session) {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(720.0f, view.x - 32.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                      ImVec2(FLT_MAX, std::max(120.0f, view.y - 32.0f)));
  ImGui::SetNextWindowBgAlpha(0.85f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Kung Fu Monkeys: online", nullptr, kFlags)) {
    const NetPhase phase = session.phase();
    const bool failed =
        phase == NetPhase::Rejected || phase == NetPhase::Disconnected || phase == NetPhase::Failed;
    const std::string status = session.statusText();
    if (failed) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", status.c_str());
    } else {
      // A small spinner so a slow handshake does not look frozen.
      constexpr const char* kSpin = "|/-\\";
      const auto spin = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % 4;
      ImGui::Text("%c %s", kSpin[spin], status.c_str());
    }
    ImGui::Separator();
    if (session.config().spectate) {
      ImGui::TextUnformatted("Spectating: no local slots requested.");
    } else {
      ImGui::Text("Local players: %u (requested slots: %u)",
                  static_cast<unsigned>(session.deviceCount()),
                  static_cast<unsigned>(session.requestedSlots()));
    }
    // The server's match (v0.24), read-only: what it runs, and who is in.
    if (session.configKnown()) {
      const sim::MatchConfig& config = session.client().matchConfig();
      ImGui::Spacing();
      ImGui::TextUnformatted("Server match (chosen by the server)");
      std::array<LobbyCard, sim::kMaxPlayers> cards{};
      for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
        LobbyCard& c = cards[i];
        const bool present = (config.players & (1u << i)) != 0;
        const SlotOwner owner = session.owner(PlayerId{i});
        c.filled = present || owner == SlotOwner::Local;
        std::snprintf(c.title, sizeof c.title, "P%u", static_cast<unsigned>(i) + 1U);
        const char* who = present ? "online" : "free";
        if (owner == SlotOwner::Local || owner == SlotOwner::Dropped) who = "you";
        std::snprintf(c.sub, sizeof c.sub, "%s", who);
      }
      drawLobbyCards(cards);
      if (ImGui::BeginTable("server_match", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        matchConfigText(session.content(), config);
        ImGui::TableNextColumn();
        drawArenaPreview(session.content(), session.content().arenaAt(config.arena),
                         config.powerUps, ImVec2(220.0f, 150.0f));
        ImGui::EndTable();
      }
    }
    ImGui::TextDisabled("The server runs the match: no pause, step or time scale online.");
    ImGui::TextDisabled("Esc: %s and return to the local lobby. F1: debug overlay",
                        failed ? "close" : "disconnect");
  }
  ImGui::End();
}

namespace {

// The local settings rows (LobbyMenu): the focused row highlighted, a click
// focuses a row, the arrow buttons change it like Left/Right.
bool drawSettingsRows(LobbyMenu& menu, const sim::ContentDB& db) {
  bool changed = false;
  const auto rows = menu.rows();
  const float valueX = 150.0f;
  const float valueW = 130.0f;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const LobbyRow& row = rows[i];
    if (row.kind == LobbyRowKind::PowerUp && row.powerUp == 0) {
      ImGui::TextDisabled("Power-ups");
    }
    ImGui::PushID(static_cast<int>(i));
    const float x0 = ImGui::GetCursorPosX();
    if (ImGui::Selectable("##row", i == menu.focus(), ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0.0f, ImGui::GetFrameHeight()))) {
      menu.setFocus(i);
    }
    ImGui::SameLine(x0 + 6.0f);
    if (row.kind == LobbyRowKind::PowerUp) {
      const uint8_t color = db.powerUpAt(row.powerUp).visual.color;
      ImGui::ColorButton("##c", slotColorVec(color), ImGuiColorEditFlags_NoTooltip,
                         ImVec2(12.0f, 12.0f));
      ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(menu.rowLabel(row).c_str());
    ImGui::SameLine(x0 + valueX);
    if (ImGui::ArrowButton("##l", ImGuiDir_Left)) {
      menu.setFocus(i);
      changed = menu.change(i, -1) || changed;
    }
    ImGui::SameLine();
    const std::string value = menu.rowValue(row);
    const float tw = ImGui::CalcTextSize(value.c_str()).x;
    const float vx = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(vx + std::max(0.0f, (valueW - tw) * 0.5f));
    ImGui::TextUnformatted(value.c_str());
    ImGui::SameLine(vx + valueW);
    if (ImGui::ArrowButton("##r", ImGuiDir_Right)) {
      menu.setFocus(i);
      changed = menu.change(i, +1) || changed;
    }
    ImGui::PopID();
  }
  return changed;
}

}  // namespace

LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu, const std::string& connectUrl) {
  const bool online = !connectUrl.empty();
  LobbyOutcome out;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(820.0f, view.x - 32.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                      ImVec2(FLT_MAX, std::max(120.0f, view.y - 32.0f)));
  ImGui::SetNextWindowBgAlpha(0.85f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Kung Fu Monkeys: lobby", nullptr, kFlags)) {
    // Player cards: who is in each slot.
    std::array<LobbyCard, sim::kMaxPlayers> cards{};
    const bool teamMode = !online && session.content().modeAt(menu.settings().mode).teams > 0;
    const sim::TeamSet teams = session.teams();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      LobbyCard& c = cards[i];
      const SlotOwner owner = session.owner(PlayerId{i});
      c.filled = owner != SlotOwner::None;
      if (teamMode && c.filled) c.team = teams[i];
      if (owner == SlotOwner::Bot) {
        std::snprintf(c.title, sizeof c.title, "BOT%u", static_cast<unsigned>(i) + 1U);
        // "bot medium", or "bot module" for a .wasm bot (its path is in F1 > Bots).
        const BotRoster& roster = session.botRoster();
        std::snprintf(c.sub, sizeof c.sub, "bot %s",
                      roster.moduleActive()
                          ? "module"
                          : std::string(ai::toString(roster.spec().tier)).c_str());
      } else if (owner == SlotOwner::None) {
        std::snprintf(c.title, sizeof c.title, "P%u", static_cast<unsigned>(i) + 1U);
        std::snprintf(c.sub, sizeof c.sub, "free");
      } else {
        std::snprintf(c.title, sizeof c.title, "P%u", static_cast<unsigned>(i) + 1U);
        shortDeviceText(session.sampler().binding(PlayerId{i}), c.sub);
      }
    }
    drawLobbyCards(cards);
    const sim::PlayerMask dropped = session.droppedMask();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      if ((dropped & (1u << i)) != 0) {
        ImGui::TextColored(slotColorVec(i), "P%u's controller disconnected",
                           static_cast<unsigned>(i) + 1U);
      }
    }

    ImGui::Spacing();
    if (online) {
      ImGui::Text("Server %s", connectUrl.c_str());
      ImGui::TextDisabled(
          "Each joined device gets its own slot; the server picks the slots, the arena, the "
          "mode, the loadout and the power-ups (shown once connected).");
    } else {
      const sim::ContentDB& db = session.content();
      const LobbySettings& ls = menu.settings();
      if (ImGui::BeginTable("lobby_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("rows", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();
        out.changed = drawSettingsRows(menu, db);
        ImGui::TableNextColumn();
        const sim::MatchConfig preview{.seed = session.seed(),
                                       .arena = ls.arena,
                                       .mode = ls.mode,
                                       .powerUps = ls.powerUps,
                                       .loadout = ls.loadout};
        const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
        drawArenaPreview(db, db.arenaAt(ls.arena), ls.powerUps, ImVec2(w, w * 0.62f));
        matchConfigText(db, preview);
        ImGui::Text("Seed 0x%llX", static_cast<unsigned long long>(session.seed()));
        ImGui::EndTable();
      }
      ImGui::TextDisabled(
          "Up/Down (W/S, d-pad, stick): choose a setting. Left/Right: change it. "
          "- / =: bots.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Join: press Throw or Dash on a free keyboard side, or any button on a pad");
    ImGui::BulletText("Keyboard left:  WASD move, J/Space throw, K/LShift dash, L/E ability");
    ImGui::BulletText("Keyboard right: arrows move, ./RCtrl throw, //RShift dash, ;/' ability");
    ImGui::BulletText(
        "Gamepad: left stick move, right stick aim, West/RB throw, South/LB dash, North/East "
        "ability");
    ImGui::BulletText("The ability button fires the third slot (loadouts such as slam)");
    ImGui::TextDisabled("Aim follows the move direction on keyboards. F1: debug overlay");

    ImGui::Spacing();
    const bool canStart = online || session.lobbyPresentMask() != 0;
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button(online ? "Connect  (Enter / Start)" : "Start match  (Enter / Start)",
                      ImVec2(-1.0f, 0.0f))) {
      out.start = true;
    }
    if (!canStart) ImGui::EndDisabled();
    if (!online && std::popcount(static_cast<uint32_t>(session.lobbyPresentMask())) < 2) {
      ImGui::TextDisabled("Rounds only end with at least two monkeys: add a player or a bot.");
    }
  }
  ImGui::End();
  return out;
}

}  // namespace kfm::client
