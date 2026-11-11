#include "hud.hpp"

#include "kfm/content/app_content.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/event_fx.hpp"
#include "kfm/presentation/palette.hpp"
#include "ui_style.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace kfm::client {

namespace {

using presentation::EffectKind;
using presentation::HudPhase;
using presentation::kNoSlot;

using ui::centredText;
using ui::kBanana;
using ui::kEmptyPip;
using ui::kShadow;
using ui::kWhite;
using ui::slotColor;
using ui::slotColorVec;
using ui::textWidth;

// v0.32: a team is drawn in the palette colour of its index (team 1 red,
// team 2 blue, ...), named "Team 1".. on screen.
[[nodiscard]] ImU32 teamColor(uint32_t team, int alpha = 255) {
  return slotColor(team, alpha);
}

// "P1".."P10" for local players, "BOT3" (slot number) for bots: several bots
// must stay distinguishable in the score bar and banners. v0.42: online, a
// slot's nickname when it has one (at most 16 bytes).
constexpr std::size_t kLabelBytes = 24;
void slotLabel(const HudView& view, uint8_t slot, char (&out)[kLabelBytes]) {
  std::snprintf(out, sizeof out, "%s",
                client::slotLabel(view.names[slot], view.owners[slot], slot).c_str());
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
      std::snprintf(text, sizeof text, "Team %u: %d", static_cast<unsigned>(t) + 1U,
                    static_cast<int>(hud.teamScores[t]));
      centredText(dl, 18.0f, ImVec2(x, y), teamColor(t), text);
      x += kTeamW;
    }
  }
  if (!hud.hasClock) return;
  char clock[16];
  clockText(hud.clockSeconds, clock);
  const bool urgent = clockUrgent(hud);
  const float pop = presentation::bannerScale(state, EffectKind::TimerBanner);
  centredText(dl, 26.0f * pop, ImVec2(viewW * 0.5f, y + 24.0f), urgent ? ui::kAlarm : kWhite,
              clock);
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

// The card every slot is drawn on, in the lobby and in the HUD: the
// panel's chunky card in small (ink border, ink drop) with the slot's
// palette colour as a stripe along the top and, in a team mode (`team` >=
// 0), the team's colour along the bottom. A dimmed card (alpha < 255) lies
// flat, without its drop.
constexpr float kCardStripe = 6.0f;
constexpr float kCardRounding = 9.0f;
constexpr float kCardBorder = 2.5f;
constexpr float kCardDrop = 3.0f;
void drawCardFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, uint8_t slot, int alpha, int team = -1,
                   float k = 1.0f) {
  const float b = kCardBorder * k;
  const float r = (kCardRounding - kCardBorder) * k;
  ui::card(dl, min, max, ui::kBark, kCardRounding * k, b, alpha < 255 ? 0.0f : kCardDrop * k,
           alpha);
  dl->AddRectFilled(ImVec2(min.x + b, min.y + b), ImVec2(max.x - b, min.y + (kCardStripe * k)),
                    slotColor(slot, alpha), r, ImDrawFlags_RoundCornersTop);
  if (team >= 0) {
    dl->AddRectFilled(ImVec2(min.x + b, max.y - b - (3.5f * k)), ImVec2(max.x - b, max.y - b),
                      teamColor(static_cast<uint32_t>(team), alpha), r,
                      ImDrawFlags_RoundCornersBottom);
  }
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
  float labelW = textWidth(kLabel, "BOT10");
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) != 0 && !view.names[i].empty()) {
      labelW = std::max(labelW, textWidth(kLabel, view.names[i].c_str()));
    }
  }
  const float scoreW = frags ? textWidth(kLabel, "-00 (0)")
                             : static_cast<float>(hud.roundsToWin) * ((kPipR * 2.0f) + 4.0f);
  const float baseW = std::max((kPad * 2.0f) + labelW + 8.0f + scoreW,
                               (kPad * 2.0f) + (static_cast<float>(timers) * kTimerStep));
  const float baseH = kCardStripe + 4.0f + kLabel + 8.0f + (timers > 0 ? 18.0f : 0.0f);
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
    // v0.32: the slot's team as a bar in the team colour along the bottom.
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + h), i, alpha,
                  hud.teamCount > 0 ? static_cast<int>(hud.teams[i]) : -1, k);
    const float lineY = y + ((kCardStripe + 4.0f) * k);
    char label[kLabelBytes];
    slotLabel(view, i, label);
    dl->AddText(font, kLabel * k, ImVec2(x + (kPad * k) + 1.0f, lineY + 1.0f), kShadow, label);
    dl->AddText(font, kLabel * k, ImVec2(x + (kPad * k), lineY), ui::withAlpha(ui::kCream, alpha),
                label);
    const float right = x + w - (kPad * k);
    if (frags) {
      char score[16];
      if (waiting) {
        std::snprintf(score, sizeof score, "%d (%d)", static_cast<int>(hud.scores[i]),
                      static_cast<int>(std::ceil(hud.respawnSeconds[i])));
      } else {
        std::snprintf(score, sizeof score, "%d", static_cast<int>(hud.scores[i]));
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
        if (std::cmp_less(pip, hud.scores[i])) {
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

// World units above the top of the head where a monkey's tag (name, ammo
// pips) is anchored: clear of the rig's round head (v0.54), whose silhouette
// rises above its top point under the tilted camera.
constexpr float kTagLift = 0.85f;

void drawPlayerTags(ImDrawList* dl, const HudView& session, const HudInput& in) {
  const presentation::RenderState& rs = *in.state;
  for (uint16_t k = 0; k < rs.playerCount; ++k) {
    const presentation::RenderPlayer& p = rs.players[k];
    // v0.57: a monkey going down a pit is out; its tag would hang over the pit.
    if (!p.visible || p.has(presentation::PlayerFlag::Falling)) continue;
    const presentation::Vec3 head =
        p.position + presentation::Vec3{0.0f, rs.playerHeight + kTagLift, 0.0f};
    const presentation::ScreenPoint sp = presentation::worldToScreen(*in.camera, head);
    if (!sp.inFront) continue;
    const ImVec2 at{sp.x * in.uiScale, sp.y * in.uiScale};

    char label[kLabelBytes];
    slotLabel(session, p.slot, label);
    centredText(dl, 15.0f, ImVec2(at.x, at.y - 16.0f), slotColor(p.slot), label);

    constexpr float kPipR = 4.0f;
    const float width = static_cast<float>(p.ammoMax) * (kPipR * 2.0f + 3.0f) - 3.0f;
    float x = at.x - (width * 0.5f) + kPipR;
    for (uint8_t a = 0; a < p.ammoMax; ++a) {
      const ImVec2 c{x, at.y};
      if (a < p.ammo) {
        dl->AddCircleFilled(c, kPipR, kBanana);
      } else {
        dl->AddCircleFilled(c, kPipR, kEmptyPip);
      }
      dl->AddCircle(c, kPipR, kShadow, 0, 1.0f);
      x += (kPipR * 2.0f) + 3.0f;
    }
    drawPowerUpDots(dl, p, ImVec2(at.x, at.y + 12.0f));
  }
}

void drawCrown(ImDrawList* dl, ImVec2 p, float w);

// v0.43: the scoreboard (core/scoreboard.hpp) as a table at `origin`,
// `width` wide: a header line, then a row per slot (place, colour, label,
// score, deaths where the mode counts them), each team's rows under a line
// with the team's name and score; the winner wears the crown and this
// client's own rows are outlined. `panel` draws a backing panel (the HUD's
// between-rounds board). `k` scales it (M7 step 6: the replay's board
// shrinks to stay clear of its control bar). Returns the height used.
float scoreTableHeight(const Scoreboard& b, float k = 1.0f) {
  return ((14.0f * 2.0f) + 24.0f + (static_cast<float>(b.rows.size() + b.teamCount) * 30.0f)) * k;
}

float drawScoreTable(ImDrawList* dl, ImVec2 origin, float width, const Scoreboard& b, bool panel,
                     float k = 1.0f) {
  const float kRowH = 30.0f * k;
  const float kPad = 14.0f * k;
  const float kText = 20.0f * k;
  const float kSmall = 16.0f * k;
  const float height = scoreTableHeight(b, k);
  if (panel) {
    ui::card(dl, origin, ImVec2(origin.x + width, origin.y + height), ui::kPanelBg,
             ui::kPanelRounding, ui::kPanelBorder, ui::kPanelDrop);
  }
  ImFont* font = ImGui::GetFont();
  const float left = origin.x + kPad;
  const float right = origin.x + width - kPad;
  const float deathsX = right;
  const float scoreX = b.showDeaths ? right - 90.0f : right;
  const auto rightText = [&](float size, float xRight, float y, ImU32 color, const char* text) {
    dl->AddText(font, size, ImVec2(xRight - textWidth(size, text), y), color, text);
  };
  float y = origin.y + kPad;
  dl->AddText(font, kSmall, ImVec2(left, y), ui::kCreamDim, "#");
  dl->AddText(font, kSmall, ImVec2(left + (52.0f * k), y), ui::kCreamDim, "Player");
  rightText(kSmall, scoreX, y, ui::kCreamDim, b.scoreLabel);
  if (b.showDeaths) rightText(kSmall, deathsX, y, ui::kCreamDim, "Deaths");
  y += 24.0f * k;
  bool firstRow = true;
  int8_t lastTeam = 0;
  for (const ScoreRow& r : b.rows) {
    char text[48];
    if (b.teamCount > 0 && (firstRow || r.team != lastTeam)) {
      lastTeam = r.team;
      const auto team = static_cast<uint32_t>(r.team < 0 ? 0 : static_cast<uint8_t>(r.team));
      std::snprintf(text, sizeof text, "Team %u", team + 1U);
      const float ty = y + ((kRowH - kText) * 0.5f);
      dl->AddRectFilled(ImVec2(left - 4.0f, y + 2.0f), ImVec2(right + 4.0f, y + kRowH - 2.0f),
                        slotColor(team, 60), 8.0f);
      ui::shadowText(dl, kText, ImVec2(left, ty), ui::kCream, text);
      std::snprintf(text, sizeof text, "%d",
                    static_cast<int>(b.teamScores[team < sim::kMaxPlayers ? team : 0]));
      rightText(kText, scoreX, ty, ui::kCream, text);
      y += kRowH;
    }
    const float ty = y + ((kRowH - kText) * 0.5f);
    if (r.local) {
      dl->AddRect(ImVec2(left - 4.0f, y + 2.0f), ImVec2(right + 4.0f, y + kRowH - 2.0f),
                  ui::withAlpha(kBanana, 170), 8.0f, 0, 1.5f);
    }
    std::snprintf(text, sizeof text, "%u.", static_cast<unsigned>(r.place));
    dl->AddText(font, kText, ImVec2(left, ty), ui::kCreamDim, text);
    // The slot's colour as a dot: the label stays cream for contrast.
    dl->AddCircleFilled(ImVec2(left + (40.0f * k), y + (kRowH * 0.5f)), 8.0f * k, ui::kOutline);
    dl->AddCircleFilled(ImVec2(left + (40.0f * k), y + (kRowH * 0.5f)), 6.0f * k,
                        slotColor(r.slot));
    float nx = left + (52.0f * k);
    if (r.winner) {
      drawCrown(dl, ImVec2(nx, ty + kText - 3.0f), 16.0f);
      nx += 22.0f;
    }
    ui::shadowText(dl, kText, ImVec2(nx, ty), ui::kCream, r.label.c_str());
    nx += textWidth(kText, r.label.c_str()) + 8.0f;
    if (r.local) dl->AddText(font, kSmall, ImVec2(nx, ty + 3.0f), ui::kCreamDim, "you");
    std::snprintf(text, sizeof text, "%d", static_cast<int>(r.score));
    rightText(kText, scoreX, ty, kWhite, text);
    if (b.showDeaths) {
      std::snprintf(text, sizeof text, "%u", static_cast<unsigned>(r.deaths));
      rightText(kText, deathsX, ty, ui::kCreamDim, text);
    }
    firstRow = false;
    y += kRowH;
  }
  return height;
}

// Banners pop in with their effect (bannerScale, event_fx.hpp). v0.43: with
// the scoreboard on screen (after a round, the match's end) the banner
// moves up and the board sits under it.
void drawBanners(ImDrawList* dl, const HudView& session, const presentation::RenderState& state,
                 ImVec2 view) {
  const presentation::RenderHud& hud = state.hud;
  if (hud.phase == HudPhase::MatchEnded && session.overScreen) return;
  const bool board = showsScoreboard(hud);
  const ImVec2 centre{view.x * 0.5f, view.y * (board ? 0.24f : 0.42f)};
  if (board) {
    const Scoreboard b =
        buildScoreboard(hud, slotLabels(session), onlineLocalMask(session), session.bots);
    const float width = std::min(560.0f, view.x - 32.0f);
    const bool over = hud.phase == HudPhase::MatchEnded;
    float top = centre.y + (over ? 120.0f : 90.0f);
    float k = 1.0f;
    // M7 step 6: clear of the replay's control bar: up under the banner,
    // then smaller (to 0.6) if it still does not fit.
    const float bottom = view.y - session.bottomInset - 8.0f;
    if (session.bottomInset > 0.0f && top + scoreTableHeight(b) > bottom) {
      top = std::max(bottom - scoreTableHeight(b), centre.y + (over ? 80.0f : 44.0f));
      k = std::clamp((bottom - top) / scoreTableHeight(b), 0.6f, 1.0f);
    }
    (void)drawScoreTable(dl, ImVec2((view.x - width) * 0.5f, top), width, b, true, k);
  }
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
      centredText(dl, 96.0f, ImVec2(centre.x, centre.y + 20.0f), ui::kGold, text);
      break;
    }
    case HudPhase::RoundEnded: {
      const float pop = presentation::bannerScale(state, EffectKind::RoundEndBanner);
      if (hud.lastRoundWinner == kNoSlot) {
        centredText(dl, 56.0f * pop, centre, kWhite, "Draw!");
      } else {
        const WinnerTitle w = winnerTitle(session, hud, static_cast<uint8_t>(hud.lastRoundWinner));
        std::snprintf(text, sizeof text, "Round won by %s", w.text.c_str());
        centredText(dl, 56.0f * pop, centre, slotColor(w.colorIndex), text);
      }
      break;
    }
    case HudPhase::MatchEnded: {
      if (hud.matchWinner != kNoSlot) {
        const WinnerTitle w = winnerTitle(session, hud, static_cast<uint8_t>(hud.matchWinner));
        std::snprintf(text, sizeof text, "%s wins the match!", w.text.c_str());
        centredText(dl, 64.0f * presentation::bannerScale(state, EffectKind::MatchEndBanner),
                    centre, slotColor(w.colorIndex), text);
      } else {
        // A tied lead when a mode's clock ran out (deathmatch).
        centredText(dl, 64.0f * presentation::bannerScale(state, EffectKind::MatchEndBanner),
                    centre, kWhite, "Draw!");
      }
      const char* hint = "Enter / Start: back to the lobby";
      if (session.network) hint = "Esc: menu (Leave room)";
      if (session.replay) hint = "End of the replay";
      centredText(dl, 20.0f, ImVec2(centre.x, centre.y + 60.0f), kWhite, hint);
      if (!session.nextMatch.empty()) {
        centredText(dl, 18.0f, ImVec2(centre.x, centre.y + 88.0f), ui::kSky,
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
  ImFont* font = ImGui::GetFont();
  const auto line = [&](float size, float top, ImU32 color, const std::string& text) {
    if (text.empty()) return;
    const ImVec2 pos{12.0f, top};
    dl->AddText(font, size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), kShadow, text.c_str());
    dl->AddText(font, size, pos, color, text.c_str());
  };
  line(15.0f, view2.y - 22.0f, ui::kSky, view.statusLine);
  // M6a: the room, large, and how to invite someone into it.
  line(15.0f, view2.y - 42.0f, ui::kSky, view.shareLine);
  line(24.0f, view2.y - (view.shareLine.empty() ? 50.0f : 70.0f), ui::kGold, view.roomLine);
}

// Short device names for the lobby cards.
void shortDeviceText(const platform::DeviceBinding& d, char (&out)[24]) {
  switch (d.kind) {
    case platform::DeviceKind::Keyboard:
      if (d.keyboardProfile == 0) {
        std::snprintf(out, sizeof out, "Keyboard + mouse");  // v0.45
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
  std::string title;  // v0.43: a nickname online
  char sub[24]{};
  int team{-1};      // v0.32: the slot's team in a team mode, -1 otherwise
  bool host{false};  // v0.43: the room's host wears the crown
  int alpha{255};    // v0.43: < 255 for a bot that only joins at the start
};

// v0.43: the room host's mark, a small banana-yellow crown at `p` (its
// bottom left), `w` wide.
void drawCrown(ImDrawList* dl, ImVec2 p, float w) {
  const float h = w * 0.7f;
  const std::array<ImVec2, 7> pts{ImVec2(p.x, p.y),
                                  ImVec2(p.x, p.y - h),
                                  ImVec2(p.x + (w * 0.27f), p.y - (h * 0.45f)),
                                  ImVec2(p.x + (w * 0.5f), p.y - h),
                                  ImVec2(p.x + (w * 0.73f), p.y - (h * 0.45f)),
                                  ImVec2(p.x + w, p.y - h),
                                  ImVec2(p.x + w, p.y)};
  dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), kBanana);
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), kShadow, ImDrawFlags_Closed, 1.0f);
}

// `perRow`: 0 picks ten in a row, or two rows of five when the window is
// narrow; the online room asks for five (wider cards for nicknames).
void drawLobbyCards(const std::array<LobbyCard, sim::kMaxPlayers>& cards, int perRowWanted = 0) {
  constexpr float kGap = 7.0f;
  constexpr float kH = 48.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  int perRow = perRowWanted;
  if (perRow == 0) perRow = avail >= (10.0f * 64.0f) + (9.0f * kGap) ? 10 : 5;
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
    const int alpha = c.filled ? c.alpha : 70;
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + kH), i, alpha, c.team);
    const ImU32 titleColor =
        c.filled ? ui::withAlpha(ui::kCream, alpha) : ui::withAlpha(ui::kCreamDim, 160);
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w - 2.0f, y + kH), true);
    const float crownW = c.host ? 18.0f : 0.0f;
    if (c.host) drawCrown(dl, ImVec2(x + 7.0f, y + kCardStripe + 17.0f), crownW - 4.0f);
    dl->AddText(font, 18.0f, ImVec2(x + 8.0f + crownW, y + kCardStripe + 1.0f), titleColor,
                c.title.c_str());
    dl->AddText(font, 14.0f, ImVec2(x + 8.0f, y + kCardStripe + 21.0f),
                c.filled ? ui::withAlpha(ui::kCream, alpha) : ui::withAlpha(ui::kCreamDim, 140),
                c.sub);
    dl->PopClipRect();
    if (c.team >= 0) {
      // v0.32: "T1".. at the right of the title and a bar in the team colour.
      const auto team = static_cast<uint32_t>(c.team);
      char tag[16];
      std::snprintf(tag, sizeof tag, "T%u", team + 1U);
      const float tw = textWidth(15.0f, tag);
      dl->AddText(font, 15.0f, ImVec2(x + w - tw - 7.0f, y + kCardStripe + 4.0f),
                  teamColor(team, alpha), tag);
    }
  }
}

// A top-down sketch of an arena from its def: floor, walls, pits (with their
// rim), slow patches, spawn points in the slot colours (spawn order) and the
// power-up spawners, filled when the match's power-up set leaves them
// something to spawn and hollow when it switches them off.
// v0.52: the player's arena theme, which the preview draws in (setHudTheme()).
presentation::ThemeChoice gTheme = presentation::ThemeChoice::Auto;

void drawArenaPreview(const sim::ContentDB& db, const sim::ArenaDef& arena, uint16_t powerUps,
                      ImVec2 box) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box.x, box.y + 4.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // On a small inset card, like everything else on a panel.
  ui::card(dl, origin, ImVec2(origin.x + box.x, origin.y + box.y), ui::kInk, 10.0f, 2.5f, 3.0f);
  const float hx = std::max(arena.halfExtents.x.toFloat(), 0.01f);
  const float hy = std::max(arena.halfExtents.y.toFloat(), 0.01f);
  const float scale = std::min((box.x - 14.0f) / (2.0f * hx), (box.y - 14.0f) / (2.0f * hy));
  const ImVec2 c{origin.x + (box.x * 0.5f), origin.y + (box.y * 0.5f)};
  const auto at = [&](float x, float y) { return ImVec2(c.x + (x * scale), c.y - (y * scale)); };
  const auto box2 = [&](Vec2 min, Vec2 max) {
    return std::pair{at(min.x.toFloat(), max.y.toFloat()), at(max.x.toFloat(), min.y.toFloat())};
  };
  // The arena's look in the player's theme (theme.hpp), as the renderer draws it.
  const presentation::ArenaLook& look =
      presentation::arenaLook(presentation::resolveTheme(gTheme, arena.name.view()));
  dl->AddRectFilled(at(-hx, hy), at(hx, -hy), toU32(look.floor));
  for (uint8_t i = 0; i < arena.hazardCount; ++i) {
    const sim::Hazard& hz = arena.hazards[i];
    const auto [a, b] = box2(hz.min, hz.max);
    if (hz.kind == sim::HazardKind::Pit) {
      dl->AddRectFilled(a, b, toU32(look.pit));
      dl->AddRect(a, b, toU32(look.pitBank), 0.0f, 0, 2.0f);
    } else {
      dl->AddRectFilled(a, b, toU32(look.mud));
    }
  }
  for (uint8_t i = 0; i < arena.wallCount; ++i) {
    const auto [a, b] = box2(arena.walls[i].min, arena.walls[i].max);
    dl->AddRectFilled(a, b, toU32(look.wall));
    dl->AddRect(a, b, toU32(look.ink), 0.0f, 0, 1.5f);
  }
  dl->AddRect(at(-hx, hy), at(hx, -hy), toU32(look.rim), 0.0f, 0, 3.0f);
  // v0.33: objective zones, translucent with a solid rim, lettered a, b, ...
  for (uint8_t i = 0; i < arena.zoneCount && i < sim::kMaxArenaZones; ++i) {
    const auto [a, b] = box2(arena.zones[i].min, arena.zones[i].max);
    dl->AddRectFilled(a, b, ui::kPreviewZone);
    dl->AddRect(a, b, ui::kPreviewZoneRim, 0.0f, 0, 1.5f);
    const char letter[2] = {static_cast<char>('a' + i), '\0'};
    dl->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), ui::kPreviewZoneLetter, letter);
  }
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
      dl->AddQuadFilled(top, right, bottom, left, ui::kPreviewSpawnerOn);
    } else {
      dl->AddQuad(top, right, bottom, left, ui::kPreviewSpawnerOff, 1.5f);
    }
  }
}

// "Label  value" with the values lined up (a proportional font), in a
// window or a table cell alike.
void labelled(const char* label, const char* value) {
  constexpr float kLabelW = 70.0f;
  const float x = ImGui::GetCursorScreenPos().x;
  ImGui::TextDisabled("%s", label);
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(x + kLabelW, ImGui::GetCursorScreenPos().y));
  ImGui::TextUnformatted(value);
}

// The match a config selects, as text: arena, mode with its key numbers,
// the loadout with its abilities, the power-up set. Shared by the online
// lobby and the local preview column.
void matchConfigText(const sim::ContentDB& db, const sim::MatchConfig& config) {
  const sim::ArenaDef& arena = db.arenaAt(config.arena);
  const sim::ModeDef& mode = db.modeAt(config.mode);
  // v0.43: a proportional font, so the values line up by position.
  const auto line = [](const char* label, const std::string& value) {
    labelled(label, value.c_str());
  };
  line("Arena", arena.name.cStr());
  line("Mode", mode.name.cStr());
  if (mode.teams > 0) {
    line("Teams", std::to_string(mode.teams) + ", balanced as players join");
  }
  ImGui::TextWrapped("%s", modeSummary(mode).c_str());
  line("Kit", std::string(db.loadoutAt(config.loadout).name.cStr()) + " (" +
                  loadoutSummary(db, config.loadout) + ")");
  if (arena.pickupSpawnerCount == 0) {
    ImGui::TextDisabled("No power-up spawners in this arena.");
  } else {
    ImGui::TextWrapped("Power-ups: %s", content::powerUpListText(db, config.powerUps).c_str());
  }
}

// The local settings rows (LobbyMenu): the focused row on the focus bar,
// a click focuses a row, the arrow buttons change it like Left/Right.
// v0.43: compact (a smaller font and spacing) and the power-up toggles two
// to a line, a click on one toggling it, so the lobby fits a 720p window in
// the game font.
bool drawSettingsRows(LobbyMenu& menu, const sim::ContentDB& db) {
  bool changed = false;
  const auto rows = menu.rows();
  constexpr float kRowFont = 17.0f;
  ImGui::PushFont(nullptr, kRowFont);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
  const float valueX = 150.0f;
  const float valueW = 140.0f;
  const float full = ImGui::GetContentRegionAvail().x;
  const float frameH = ImGui::GetFrameHeight();
  int powerUpColumn = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const LobbyRow& row = rows[i];
    const bool focused = i == menu.focus();
    ImGui::PushID(static_cast<int>(i));
    if (row.kind == LobbyRowKind::PowerUp) {
      if (powerUpColumn == 0) {
        ImGui::TextDisabled("Power-ups");
      } else if (powerUpColumn % 2 == 1) {
        ImGui::SameLine(0.0f, 8.0f);
      }
      ++powerUpColumn;
      // One toggle: the colour, the name, on/off at the right.
      const float w = (full - 8.0f) * 0.5f;
      const ImVec2 min = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##toggle", ImVec2(w, frameH))) {
        menu.setFocus(i);
        changed = menu.change(i, +1) || changed;
      }
      if (ui::pointerTakesFocus()) menu.setFocus(i);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const bool on = (menu.settings().powerUps & (1u << row.powerUp)) != 0;
      const ImVec2 max{min.x + w, min.y + frameH};
      // v0.55: highlighted by the menu's device (ui::showsFocus).
      if (ui::showsFocus(focused, ui::pointerOverItem())) {
        ui::focusBar(min, max);
      } else {
        ui::card(dl, min, max, ui::kBarkInset, 10.0f, 2.0f, 0.0f);
      }
      const uint8_t color = db.powerUpAt(row.powerUp).visual.color;
      dl->AddCircleFilled(ImVec2(min.x + 12.0f, min.y + (frameH * 0.5f)), 5.0f, slotColor(color));
      const float textY = min.y + ((frameH - kRowFont) * 0.5f);
      dl->AddText(ImGui::GetFont(), kRowFont, ImVec2(min.x + 24.0f, textY),
                  on ? ui::kCream : ui::kCreamDim, menu.rowLabel(row).c_str());
      const char* state = on ? "on" : "off";
      dl->AddText(ImGui::GetFont(), kRowFont,
                  ImVec2(max.x - textWidth(kRowFont, state) - 10.0f, textY),
                  on ? ui::kMangoLight : ui::kCreamDim, state);
      ImGui::PopID();
      continue;
    }
    powerUpColumn = 0;
    const float x0 = ImGui::GetCursorPosX();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Header, ui::vec4(ui::kTransparent));
    // v0.55: no hover tint of its own: the focus bar is the highlight.
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::vec4(ui::kTransparent));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::vec4(ui::withAlpha(ui::kMango, 140)));
    if (ImGui::Selectable("##row", focused, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0.0f, frameH))) {
      menu.setFocus(i);
    }
    ImGui::PopStyleColor(3);
    const ImVec2 barMin{min.x - 2.0f, min.y - 1.0f};
    const ImVec2 barMax{min.x + full + 2.0f, min.y + frameH + 1.0f};
    const bool over = ui::pointerOverRect(barMin, barMax);
    if (ui::pointerTakesFocus(over)) menu.setFocus(i);
    if (ui::showsFocus(focused, over)) ui::focusBar(barMin, barMax);
    ImGui::SameLine(x0 + 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(menu.rowLabel(row).c_str());
    ImGui::SameLine(x0 + valueX);
    if (ui::arrowButton("##l", ImGuiDir_Left)) {
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
    if (ui::arrowButton("##r", ImGuiDir_Right)) {
      menu.setFocus(i);
      changed = menu.change(i, +1) || changed;
    }
    ImGui::PopID();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  return changed;
}

// v0.43: the room's code, large, with Copy link and (touch) Share.
ShareOutcome drawShare(const ShareView& share) {
  ShareOutcome out;
  if (share.code.empty()) return out;
  ImGui::PushFont(nullptr, ui::kFontUi);
  ImGui::TextDisabled("Room code (%s)", share.region.c_str());
  ImGui::PopFont();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushFont(nullptr, ui::kFontTitle);
  ui::shadowText(ImGui::GetWindowDrawList(), ui::kFontTitle, at, ui::kGold, share.code.c_str());
  ImGui::Dummy(ImVec2(textWidth(ui::kFontTitle, share.code.c_str()), ImGui::GetFontSize()));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 18.0f);
  // Without a page URL (natively, unless the build bakes one) the link is
  // the code itself: friends type it into Play online.
  const bool codeOnly = share.link == share.code;
  ImGui::BeginGroup();
  if (ui::bigButton(codeOnly ? "Copy code" : "Copy link", false, 0.0f, true, ui::kFontUi + 2.0f,
                    ui::Tone::Second)) {
    out.copy = true;
  }
  if (share.canShare) {
    ImGui::SameLine();
    if (ui::bigButton("Share", false, 0.0f, true, ui::kFontUi + 2.0f)) out.share = true;
  }
  if (share.copied) {
    ImGui::TextColored(ui::vec4(ui::kMangoLight), "Copied: %s", share.link.c_str());
  } else if (codeOnly) {
    ImGui::TextDisabled("Friends enter it in Play online > Room code");
  } else {
    ImGui::TextDisabled("%s", share.link.c_str());
  }
  ImGui::EndGroup();
  return out;
}

}  // namespace

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

NetStatusOutcome drawNetStatus(const NetSession& session, const ShareView& share,
                               ButtonMenu& buttons) {
  NetStatusOutcome out;
  if (ui::beginScreen("##online_status", 760.0f)) {
    const NetPhase phase = session.phase();
    const bool failed =
        phase == NetPhase::Rejected || phase == NetPhase::Disconnected || phase == NetPhase::Failed;
    const bool lost = session.canReconnect();
    const char* title = session.room() ? "Joining the room" : "Connecting";
    if (session.reconnects() > 0) title = "Reconnecting";
    if (failed) title = "Could not join";
    if (lost) title = "Connection lost";
    ui::heading(title, failed ? ui::kAlarm : ui::kGold);
    const std::string status = session.statusText();
    if (failed) {
      ImGui::TextColored(ui::vec4(ui::kAlarm), "%s", status.c_str());
    } else {
      // A small spinner so a slow handshake does not look frozen.
      constexpr const char* kSpin = "|/-\\";
      const auto spin = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % 4;
      ImGui::Text("%c  %s", kSpin[spin], status.c_str());
    }
    // v0.44: back in with the same token: the server keeps the slots of a
    // match in progress for a few seconds.
    if (lost) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Your place in the match is kept for a few seconds.");
      const std::span<const LostConnectionAction> actions = lostConnectionActions(lost);
      const std::size_t n = actions.size();
      buttons.setCount(n);
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      const float bw =
          n == 0 ? 0.0f
                 : (ImGui::GetContentRegionAvail().x - (static_cast<float>(n - 1) * gap)) /
                       static_cast<float>(n);
      for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) ImGui::SameLine();
        // The first button is the screen's lead (Reconnect).
        if (ui::bigButton(lostConnectionLabel(actions[i]), i == buttons.focus(), bw, true,
                          ui::kFontButton, i == 0 ? ui::Tone::Lead : ui::Tone::Plain)) {
          buttons.setFocus(i);
          out.action = actions[i];
        }
        if (ui::pointerTakesFocus()) buttons.setFocus(i);
      }
    }
    // M6a: the room the lobby gave, large, and the invite to copy.
    if (session.room()) {
      ImGui::Spacing();
      out.share = drawShare(share);
    }
    ImGui::Separator();
    if (session.config().spectate) {
      ImGui::TextUnformatted("Spectating: no local slots requested.");
    } else {
      ImGui::Text("Local players: %u (requested slots: %u)",
                  static_cast<unsigned>(session.deviceCount()),
                  static_cast<unsigned>(session.requestedSlots()));
    }
    // The server's match (v0.24), read-only: what it runs, and who is in
    // (a server on the M3 flow, without a room).
    if (session.configKnown() && !session.roomState()) {
      const sim::MatchConfig& config = session.client().matchConfig();
      ImGui::Spacing();
      ImGui::TextUnformatted("Server match (chosen by the server)");
      std::array<LobbyCard, sim::kMaxPlayers> cards{};
      for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
        LobbyCard& c = cards[i];
        const bool present = (config.players & (1u << i)) != 0;
        const SlotOwner owner = session.owner(PlayerId{i});
        c.filled = present || owner == SlotOwner::Local;
        c.title = "P" + std::to_string(i + 1);
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
      ImGui::TextDisabled("The server runs the match: no pause, step or time scale online.");
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Esc: %s and go back to the menu. F1: debug overlay",
                        failed ? "close" : "disconnect");
    ImGui::PopFont();
  }
  ui::endScreen();
  return out;
}

LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu) {
  LobbyOutcome out;
  if (ui::beginScreen("##local_lobby", 900.0f)) {
    ui::heading("Local play");
    // Player cards: who is in each slot.
    std::array<LobbyCard, sim::kMaxPlayers> cards{};
    const bool teamMode = session.content().modeAt(menu.settings().mode).teams > 0;
    const sim::TeamSet teams = session.teams();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      LobbyCard& c = cards[i];
      const SlotOwner owner = session.owner(PlayerId{i});
      c.filled = owner != SlotOwner::None;
      if (teamMode && c.filled) c.team = teams[i];
      c.title = client::slotLabel({}, owner, i);
      if (owner == SlotOwner::Bot) {
        // "bot medium", or "bot module" for a .wasm bot (its path is in F1 > Bots).
        const BotRoster& roster = session.botRoster();
        std::snprintf(c.sub, sizeof c.sub, "bot %s",
                      roster.moduleActive()
                          ? "module"
                          : std::string(ai::toString(roster.spec().tier)).c_str());
      } else if (owner == SlotOwner::None) {
        std::snprintf(c.sub, sizeof c.sub, "free");
      } else {
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
      ImGui::PushFont(nullptr, ui::kFontSmall);
      const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
      drawArenaPreview(db, db.arenaAt(ls.arena), ls.powerUps, ImVec2(w, w * 0.55f));
      matchConfigText(db, preview);
      ImGui::TextDisabled("Seed 0x%llX", static_cast<unsigned long long>(session.seed()));
      ImGui::PopFont();
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextUnformatted(
        "Join: press Throw or Dash (or click) on a free keyboard side, or any button on a pad");
    ImGui::BulletText("Throw: hold to charge (further and faster), release to throw");
    // Settings > Mouse aim (v0.55): off, the move direction aims.
    ImGui::BulletText(
        "Keyboard + mouse: WASD move, %s, left click/J/Space throw, right "
        "click/K/LShift dash, L/E ability",
        session.sampler().mouseAimEnabled() ? "mouse aims" : "movement aims (mouse aim is off)");
    ImGui::BulletText("Keyboard right: arrows move, ./RCtrl throw, //RShift dash, ;/' ability");
    ImGui::BulletText(
        "Gamepad: left stick move, right stick aim, West/RB throw, South/LB dash, North/East "
        "ability");
    ImGui::TextDisabled(
        "Up/Down (W/S, d-pad, stick): choose a setting. Left/Right: change it. - / =: bots. "
        "Esc: menu. F1: debug overlay");
    ImGui::PopFont();

    ImGui::Spacing();
    const bool canStart = session.lobbyPresentMask() != 0;
    if (ui::bigButton("Start match  (Enter / Start)", false, -1.0f, canStart, ui::kFontButton,
                      ui::Tone::Lead)) {
      out.start = true;
    }
    if (std::popcount(static_cast<uint32_t>(session.lobbyPresentMask())) < 2) {
      ImGui::TextDisabled("Rounds only end with at least two monkeys: add a player or a bot.");
    }
  }
  ui::endScreen();
  return out;
}

// --- the online room (v0.42, restyled in v0.43) ------------------------------------------

RoomOutcome drawRoomScreen(const NetSession& session, LobbyMenu& menu, const ShareView& share) {
  RoomOutcome out;
  const std::optional<net::RoomStateMsg>& room = session.roomState();
  if (!room) return out;
  const bool host = session.isHost();
  const sim::ContentDB& db = session.content();
  if (ui::beginScreen("##room", 900.0f)) {
    if (share.code.empty()) {
      ui::heading("Room");
    } else {
      out.share = drawShare(share);
    }
    ImGui::Spacing();

    // The roster (roomSlots()): the humans by name (the host's crown, you),
    // the bots where the server says it puts them at the start, then the
    // open slots.
    std::array<LobbyCard, sim::kMaxPlayers> cards{};
    const net::RoomSettings& r = room->settings;
    const std::array<RoomSlot, sim::kMaxPlayers> slots = roomSlots(*room);
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      const PlayerId slot{i};
      LobbyCard& c = cards[i];
      c.filled = slots[i] != RoomSlot::Open;
      switch (slots[i]) {
        case RoomSlot::Human: {
          c.title = session.slotName(slot);
          c.host = room->host == slot;
          const bool you = sim::maskHas(session.localSlots(), slot);
          const char* role = c.host ? "host" : "player";
          if (you) role = c.host ? "you, host" : "you";
          std::snprintf(c.sub, sizeof c.sub, "%s", role);
          break;
        }
        case RoomSlot::Bot:
          c.title = session.slotName(slot);
          std::snprintf(c.sub, sizeof c.sub, "bot");
          break;
        case RoomSlot::BotAtStart:
          c.alpha = 150;
          c.title = "BOT" + std::to_string(i + 1);
          std::snprintf(c.sub, sizeof c.sub, "joins at the start");
          break;
        case RoomSlot::Open:
          c.title = "open";
          std::snprintf(c.sub, sizeof c.sub, "send the link");
          break;
      }
    }
    drawLobbyCards(cards, 5);
    ImGui::Spacing();

    // The match: the host changes it, everyone else sees it.
    const sim::MatchConfig shown{
        .arena = r.arena, .mode = r.mode, .powerUps = r.powerUps, .loadout = r.loadout};
    if (ImGui::BeginTable("room_match", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("rows", ImGuiTableColumnFlags_WidthStretch, 1.15f);
      ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextColumn();
      if (host) {
        out.settingsChanged = drawSettingsRows(menu, db);
      } else {
        matchConfigText(db, shown);
        labelled("Bots", std::to_string(r.bots).c_str());
      }
      ImGui::TableNextColumn();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
      drawArenaPreview(db, db.arenaAt(r.arena), r.powerUps, ImVec2(w, w * 0.55f));
      if (host) matchConfigText(db, shown);
      ImGui::PopFont();
      ImGui::EndTable();
    }

    ImGui::Spacing();
    const float leaveW = 160.0f;
    if (host) {
      if (ui::bigButton("Start match  (Enter)", true,
                        ImGui::GetContentRegionAvail().x - leaveW - ImGui::GetStyle().ItemSpacing.x,
                        true, ui::kFontButton, ui::Tone::Lead)) {
        out.start = true;
      }
    } else {
      const std::string hostName =
          room->host.isValid() ? session.slotName(room->host) : std::string("the host");
      const std::string line = "Waiting for " + hostName + " to start the match";
      ImGui::PushFont(nullptr, ui::kFontButton);
      constexpr const char* kDots[] = {"", ".", "..", "..."};
      const auto dots = static_cast<std::size_t>(ImGui::GetTime() * 2.0) % 4;
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ui::vec4(ui::kGold), "%s%s", line.c_str(), kDots[dots]);
      ImGui::PopFont();
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - leaveW);
    }
    if (host) ImGui::SameLine();
    if (ui::bigButton("Leave", false, leaveW)) out.leave = true;
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("%s", host ? "Up/Down: choose a setting. Left/Right: change it. Enter: "
                                     "start. Esc: leave the room. F1: debug overlay"
                                   : "The host picks the match. Esc: leave the room. F1: debug "
                                     "overlay");
    ImGui::PopFont();
  }
  ui::endScreen();
  return out;
}

std::optional<MatchOverAction> drawMatchOver(const MatchOverView& view, ButtonMenu& menu) {
  std::optional<MatchOverAction> clicked;
  menu.setCount(view.actions.size());
  if (ui::beginScreen("##match_over", 640.0f)) {
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Match over");
    ImGui::PopFont();
    ui::heading(view.title.c_str(), view.colorIndex ? slotColor(*view.colorIndex) : kWhite,
                ui::kFontHeading + 6.0f);
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = drawScoreTable(ImGui::GetWindowDrawList(), at, width, view.board, false);
    ImGui::Dummy(ImVec2(width, h));
    if (!view.waiting.empty()) {
      ImGui::PushFont(nullptr, ui::kFontButton);
      ImGui::TextColored(ui::vec4(ui::kGold), "%s", view.waiting.c_str());
      ImGui::PopFont();
    }
    const std::size_t n = view.actions.size();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float bw =
        n == 0 ? 0.0f : (width - (static_cast<float>(n - 1) * gap)) / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
      if (i > 0) ImGui::SameLine();
      // The first button is the screen's lead (Play again).
      if (ui::bigButton(matchOverLabel(view.actions[i]), i == menu.focus(), bw, true,
                        ui::kFontButton, i == 0 ? ui::Tone::Lead : ui::Tone::Plain)) {
        menu.setFocus(i);
        clicked = view.actions[i];
      }
      if (ui::pointerTakesFocus()) menu.setFocus(i);
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Left/Right: choose. Enter / A: select.");
    ImGui::PopFont();
  }
  ui::endScreen();
  return clicked;
}

namespace {

// The replay's timeline: a track with the played part in mango and a cream
// knob at the current tick, `w` wide and as tall as the bar's buttons. A
// click or a drag seeks (ReplayControls::scrubTo); the release seeks once
// more (endScrub) so the session's clock restarts there.
void drawReplayTimeline(ReplayControls& controls, const ReplayStatus& s, float w, float faceH,
                        ReplayBarOutcome& out) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  (void)ImGui::InvisibleButton("##timeline", ImVec2(std::max(w, 1.0f), faceH + ui::kButtonDrop));
  const bool over = ui::pointerOverItem();
  if (ui::pointerTakesFocus()) controls.setFocus(ReplayControl::Timeline);
  constexpr float kKnobR = 10.0f;
  constexpr float kTrackH = 12.0f;
  const float x0 = at.x + kKnobR + ui::kFocusRingW;
  const float trackW = std::max(1.0f, w - (2.0f * (kKnobR + ui::kFocusRingW)));
  if (ImGui::IsItemActive() && s.tickCount > 0) {
    controls.setFocus(ReplayControl::Timeline);
    const float f = (ImGui::GetIO().MousePos.x - x0) / trackW;
    if (const std::optional<ReplayCommand> cmd = controls.scrubTo(replayTickAt(f, s.tickCount))) {
      out.command = *cmd;
    }
  } else if (const std::optional<ReplayCommand> cmd = controls.endScrub()) {
    out.command = *cmd;
  }
  out.scrubbing = controls.scrubbing();
  // While held, the knob follows the pointer's tick (sought this frame).
  const uint32_t shown =
      out.command.kind == ReplayCommand::Kind::SeekTo ? out.command.tick : s.tick;
  const float frac =
      s.tickCount == 0 ? 0.0f : static_cast<float>(shown) / static_cast<float>(s.tickCount);
  const float cy = at.y + (faceH * 0.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (ui::showsFocus(controls.focus() == ReplayControl::Timeline, over)) {
    ui::focusRing(ImVec2(at.x, cy - kKnobR - 4.0f), ImVec2(at.x + w, cy + kKnobR + 4.0f),
                  kKnobR + 4.0f);
  }
  const ImVec2 lo{x0, cy - (kTrackH * 0.5f)};
  const ImVec2 hi{x0 + trackW, cy + (kTrackH * 0.5f)};
  ui::card(dl, lo, hi, ui::kBarkInset, kTrackH * 0.5f, 2.0f, 0.0f);
  const float knobX = x0 + (trackW * frac);
  if (knobX > lo.x + 2.0f) {
    dl->AddRectFilled(ImVec2(lo.x + 2.0f, lo.y + 2.0f), ImVec2(knobX, hi.y - 2.0f), ui::kMango,
                      (kTrackH * 0.5f) - 2.0f);
  }
  dl->AddCircleFilled(ImVec2(knobX, cy + 2.0f), kKnobR + 2.5f, ui::kOutline);
  dl->AddCircleFilled(ImVec2(knobX, cy), kKnobR + 2.5f, ui::kOutline);
  ImU32 knob = over ? ui::kSandLight : ui::kCream;
  if (ImGui::IsItemActive()) knob = ui::kGold;
  dl->AddCircleFilled(ImVec2(knobX, cy), kKnobR, knob);
}

// The bar's geometry on the current display: the pieces measured in the
// buttons' font, the rows by replayBarRows (core/replay_controls.hpp).
struct ReplayBarLayout {
  static constexpr float kMargin = 16.0f;
  static constexpr float kPad = 14.0f;
  static constexpr float kRowGap = 10.0f;
  static constexpr float kGap = 10.0f;                // between items on a row
  static constexpr float kSize = ui::kFontUi + 2.0f;  // the buttons' font
  float faceH{0.0f};
  float rowH{0.0f};
  float width{0.0f};
  // Fixed widths, so the bar does not shift when a label changes.
  float restartW{0.0f};
  float playW{0.0f};
  float stepW{0.0f};
  float speedW{0.0f};
  float menuW{0.0f};
  float timeW{0.0f};
  ReplayBarRows rows{};
  float titleH{0.0f};
  float height{0.0f};
};

[[nodiscard]] float replayButtonWidth(const char* text) {
  const float padX = std::round(ReplayBarLayout::kSize * 0.6f);
  return textWidth(ReplayBarLayout::kSize, text) + (2.0f * (padX + ui::kButtonBorder));
}

[[nodiscard]] ReplayBarLayout replayBarLayout(bool titled) {
  ReplayBarLayout l;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  l.faceH = ui::bigButtonHeight(ReplayBarLayout::kSize);
  l.rowH = l.faceH + ui::kButtonDrop;
  l.width = std::max(display.x - (2.0f * ReplayBarLayout::kMargin), 1.0f);
  l.restartW = replayButtonWidth("Restart");
  l.playW = std::max(
      {replayButtonWidth("Watch again"), replayButtonWidth("Pause"), replayButtonWidth("Play")});
  l.stepW = replayButtonWidth("Step");
  l.speedW = replayButtonWidth("0.25x");
  l.menuW = replayButtonWidth("Main menu");
  l.timeW = textWidth(ui::kFontUi, "00:00 / 00:00");
  constexpr float kGap = ReplayBarLayout::kGap;
  l.rows = replayBarRows(
      ReplayBarPieces{.inner = l.width - (2.0f * ReplayBarLayout::kPad),
                      .transport = l.restartW + l.playW + l.stepW + l.speedW + (3.0f * kGap),
                      .time = l.timeW,
                      .menu = l.menuW,
                      .gap = kGap});
  l.titleH = titled ? ui::kFontSmall + 6.0f : 0.0f;
  l.height = (2.0f * ReplayBarLayout::kPad) + l.titleH + l.rowH +
             (l.rows.twoRows ? l.rowH + ReplayBarLayout::kRowGap : 0.0f);
  return l;
}

}  // namespace

float replayBarInset(bool titled) {
  return replayBarLayout(titled).height + ReplayBarLayout::kMargin + ui::kPanelDrop;
}

ReplayBarOutcome drawReplayBar(ReplayControls& controls, const ReplayBarView& view) {
  ReplayBarOutcome out;
  const ReplayStatus& s = view.status;
  controls.refocus(s);  // Step disabled under the focus: Play / Pause has it
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const ReplayBarLayout layout = replayBarLayout(!view.title.empty());
  constexpr float kMargin = ReplayBarLayout::kMargin;
  constexpr float kPad = ReplayBarLayout::kPad;
  constexpr float kRowGap = ReplayBarLayout::kRowGap;
  constexpr float kSize = ReplayBarLayout::kSize;
  const float faceH = layout.faceH;
  const float rowH = layout.rowH;
  const float width = layout.width;
  const bool twoRows = layout.rows.twoRows;
  const float titleH = layout.titleH;
  const float height = layout.height;
  ImGui::SetNextWindowPos(ImVec2(kMargin, display.y - kMargin - ui::kPanelDrop - height),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, kPad));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ReplayBarLayout::kGap, kRowGap));
  const bool open = ImGui::Begin("##replay_bar", nullptr, kFlags);
  {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 sz = ImGui::GetWindowSize();
    ui::card(ImGui::GetBackgroundDrawList(), pos, ImVec2(pos.x + sz.x, pos.y + sz.y), ui::kPanelBg,
             ui::kPanelRounding, ui::kPanelBorder, ui::kPanelDrop);
  }
  if (open) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!view.title.empty()) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      dl->AddText(ImGui::GetFont(), ui::kFontSmall, at, ui::kCreamDim, view.title.c_str());
      ImGui::Dummy(ImVec2(1.0f, titleH - ImGui::GetStyle().ItemSpacing.y));
    }
    const float menuW = layout.menuW;
    const float timeW = layout.timeW;
    const std::string time = replayTimeLabel(s.tick, s.tickCount);
    const auto button = [&](ReplayControl c, float w, ui::Tone tone) {
      const bool enabled = ReplayControls::enabled(c, s);
      if (ui::bigButton(ReplayControls::label(c, s), controls.focus() == c, w, enabled, kSize,
                        tone)) {
        controls.setFocus(c);
        out.command = ReplayControls::activate(c, s);
      }
      if (enabled && ui::pointerTakesFocus()) controls.setFocus(c);
    };
    const auto timeText = [&] {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(timeW, rowH));
      const float tw = textWidth(ui::kFontUi, time.c_str());
      ui::shadowText(dl, ui::kFontUi,
                     ImVec2(at.x + timeW - tw, at.y + ((faceH - ui::kFontUi) * 0.5f)), ui::kCream,
                     time.c_str());
    };
    const auto transport = [&] {
      button(ReplayControl::Restart, layout.restartW, ui::Tone::Plain);
      ImGui::SameLine();
      button(ReplayControl::PlayPause, layout.playW, ui::Tone::Lead);
      ImGui::SameLine();
      button(ReplayControl::Step, layout.stepW, ui::Tone::Plain);
      ImGui::SameLine();
      button(ReplayControl::Speed, layout.speedW, ui::Tone::Second);
    };
    if (twoRows) {
      drawReplayTimeline(controls, s, layout.rows.timeline, faceH, out);
      ImGui::SameLine();
      timeText();
      transport();
      ImGui::SameLine();
      const float rest = ImGui::GetContentRegionAvail().x;
      if (rest > menuW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rest - menuW);
      button(ReplayControl::MainMenu, menuW, ui::Tone::Plain);
    } else {
      transport();
      ImGui::SameLine();
      drawReplayTimeline(controls, s, layout.rows.timeline, faceH, out);
      ImGui::SameLine();
      timeText();
      ImGui::SameLine();
      button(ReplayControl::MainMenu, menuW, ui::Tone::Plain);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  return out;
}

std::optional<PauseItem> drawPauseMenu(PauseMenu& menu) {
  std::optional<PauseItem> clicked;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Over the scene and the HUD (both on the background list), under the panel.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display, ui::kScrim);
  if (ui::beginScreen("##pause_menu", 380.0f)) {
    ui::heading("Paused");
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("%s", menu.online() ? "The match goes on: your monkey stands still."
                                            : "The match waits for you.");
    ImGui::PopFont();
    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const std::span<const PauseItem> items = menu.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (ui::bigButton(PauseMenu::label(items[i]), i == menu.focus(), width, true, ui::kFontButton,
                        items[i] == PauseItem::Resume ? ui::Tone::Lead : ui::Tone::Plain)) {
        menu.setFocus(i);
        clicked = items[i];
      }
      // Pointer mode: the button under the pointer takes the focus (v0.55,
      // ui::pointerTakesFocus), as on the menu screens.
      if (ui::pointerTakesFocus()) menu.setFocus(i);
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Up/Down: choose. Enter / A: select. Esc / B: resume.");
    ImGui::PopFont();
  }
  ui::endScreen();
  return clicked;
}

void setHudTheme(presentation::ThemeChoice choice) {
  gTheme = choice;
}

}  // namespace kfm::client
