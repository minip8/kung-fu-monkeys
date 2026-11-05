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
#include <cstdio>
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

// The card every slot is drawn on, in the lobby and in the HUD: a dark
// rounded box with the slot's palette colour as a stripe along the top.
constexpr float kCardStripe = 4.0f;
constexpr float kCardRounding = 6.0f;
void drawCardFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, uint8_t slot, int alpha) {
  dl->AddRectFilled(min, max, ui::withAlpha(ui::kJungle, (190 * alpha) / 255), kCardRounding);
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
  float labelW = textWidth(kLabel, "BOT10");
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) != 0 && !view.names[i].empty()) {
      labelW = std::max(labelW, textWidth(kLabel, view.names[i].c_str()));
    }
  }
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
    char label[kLabelBytes];
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
                char (&out)[kLabelBytes], ImU32& color) {
  if (hud.teamCount > 0 && winner < sim::kMaxPlayers) {
    std::snprintf(out, sizeof out, "Team %u", static_cast<unsigned>(hud.teams[winner]) + 1U);
    color = teamColor(hud.teams[winner]);
    return;
  }
  char label[kLabelBytes];
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

// The rows the scoreboard marks "you": this client's slots online; none in
// local play, where several players share the screen.
[[nodiscard]] sim::PlayerMask onlineLocalMask(const HudView& view) {
  if (!view.network) return 0;
  sim::PlayerMask m = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (view.owners[i] == SlotOwner::Local || view.owners[i] == SlotOwner::Dropped) {
      m = static_cast<sim::PlayerMask>(m | (1u << i));
    }
  }
  return m;
}

// v0.43: the scoreboard (core/scoreboard.hpp) as a table at `origin`,
// `width` wide: a header line, then a row per slot (place, colour, label,
// score, deaths where the mode counts them), each team's rows under a line
// with the team's name and score; the winner wears the crown and this
// client's own rows are outlined. `panel` draws a backing panel (the HUD's
// between-rounds board). Returns the height used.
float drawScoreTable(ImDrawList* dl, ImVec2 origin, float width, const Scoreboard& b, bool panel) {
  constexpr float kRowH = 30.0f;
  constexpr float kPad = 14.0f;
  constexpr float kText = 20.0f;
  constexpr float kSmall = 16.0f;
  const std::size_t teamLines = b.teamCount;
  const float height =
      (kPad * 2.0f) + 24.0f + (static_cast<float>(b.rows.size() + teamLines) * kRowH);
  if (panel) {
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      ui::withAlpha(ui::kJungle, 215), 14.0f);
    dl->AddRect(origin, ImVec2(origin.x + width, origin.y + height), ui::withAlpha(ui::kBark, 220),
                14.0f, 0, 2.0f);
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
  dl->AddText(font, kSmall, ImVec2(left + 52.0f, y), ui::kCreamDim, "Player");
  rightText(kSmall, scoreX, y, ui::kCreamDim, b.scoreLabel);
  if (b.showDeaths) rightText(kSmall, deathsX, y, ui::kCreamDim, "Deaths");
  y += 24.0f;
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
      ui::shadowText(dl, kText, ImVec2(left, ty), slotColor(team), text);
      std::snprintf(text, sizeof text, "%u",
                    static_cast<unsigned>(b.teamScores[team < sim::kMaxPlayers ? team : 0]));
      rightText(kText, scoreX, ty, slotColor(team), text);
      y += kRowH;
    }
    const float ty = y + ((kRowH - kText) * 0.5f);
    if (r.local) {
      dl->AddRect(ImVec2(left - 4.0f, y + 2.0f), ImVec2(right + 4.0f, y + kRowH - 2.0f),
                  ui::withAlpha(kBanana, 170), 8.0f, 0, 1.5f);
    }
    std::snprintf(text, sizeof text, "%u.", static_cast<unsigned>(r.place));
    dl->AddText(font, kText, ImVec2(left, ty), ui::kCreamDim, text);
    dl->AddCircleFilled(ImVec2(left + 40.0f, y + (kRowH * 0.5f)), 6.0f, slotColor(r.slot));
    float nx = left + 52.0f;
    if (r.winner) {
      drawCrown(dl, ImVec2(nx, ty + kText - 3.0f), 16.0f);
      nx += 22.0f;
    }
    ui::shadowText(dl, kText, ImVec2(nx, ty), slotColor(r.slot), r.label.c_str());
    nx += textWidth(kText, r.label.c_str()) + 8.0f;
    if (r.local) dl->AddText(font, kSmall, ImVec2(nx, ty + 3.0f), ui::kCreamDim, "you");
    std::snprintf(text, sizeof text, "%u", static_cast<unsigned>(r.score));
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
// the scoreboard on screen (between rounds, the match's end) the banner
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
    const float top = centre.y + (hud.phase == HudPhase::MatchEnded ? 120.0f : 90.0f);
    (void)drawScoreTable(dl, ImVec2((view.x - width) * 0.5f, top), width, b, true);
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
      centredText(dl, 96.0f, ImVec2(centre.x, centre.y + 20.0f), kBanana, text);
      break;
    }
    case HudPhase::RoundEnded: {
      const float pop = presentation::bannerScale(state, EffectKind::RoundEndBanner);
      if (hud.lastRoundWinner == kNoSlot) {
        centredText(dl, 56.0f * pop, centre, kWhite, "Draw!");
      } else {
        const auto w = static_cast<uint8_t>(hud.lastRoundWinner);
        char label[kLabelBytes];
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
        char label[kLabelBytes];
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
                  session.network ? "Esc: menu (Leave room)" : "Enter / Start: back to the lobby");
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
  line(24.0f, view2.y - (view.shareLine.empty() ? 50.0f : 70.0f), kBanana, view.roomLine);
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
  constexpr float kGap = 6.0f;
  constexpr float kH = 44.0f;
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
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + kH), i, alpha);
    const ImU32 titleColor = c.filled ? slotColor(i, alpha) : ui::withAlpha(ui::kCreamDim, 160);
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w - 2.0f, y + kH), true);
    const float crownW = c.host ? 18.0f : 0.0f;
    if (c.host) drawCrown(dl, ImVec2(x + 7.0f, y + kCardStripe + 18.0f), crownW - 4.0f);
    dl->AddText(font, 18.0f, ImVec2(x + 7.0f + crownW, y + kCardStripe + 2.0f), titleColor,
                c.title.c_str());
    dl->AddText(font, 14.0f, ImVec2(x + 7.0f, y + kCardStripe + 22.0f),
                c.filled ? ui::withAlpha(ui::kCream, alpha) : ui::withAlpha(ui::kCreamDim, 140),
                c.sub);
    dl->PopClipRect();
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
  // v0.33: objective zones, translucent with a solid rim, lettered a, b, ...
  for (uint8_t i = 0; i < arena.zoneCount && i < sim::kMaxArenaZones; ++i) {
    const auto [a, b] = box2(arena.zones[i].min, arena.zones[i].max);
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 50));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 190), 0.0f, 0, 1.5f);
    const char letter[2] = {static_cast<char>('a' + i), '\0'};
    dl->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), IM_COL32(255, 255, 255, 220), letter);
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
      dl->AddQuadFilled(top, right, bottom, left, IM_COL32(255, 255, 255, 230));
    } else {
      dl->AddQuad(top, right, bottom, left, IM_COL32(170, 170, 170, 200), 1.5f);
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

// The local settings rows (LobbyMenu): the focused row behind a banana bar,
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
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const bool on = (menu.settings().powerUps & (1u << row.powerUp)) != 0;
      const ImVec2 max{min.x + w, min.y + frameH};
      dl->AddRectFilled(min, max,
                        ImGui::IsItemHovered() ? ui::withAlpha(ui::kLeaf, 90)
                                               : ui::withAlpha(ui::kJungleLight, 160),
                        8.0f);
      if (focused) ui::focusBar(min, max);
      const uint8_t color = db.powerUpAt(row.powerUp).visual.color;
      dl->AddCircleFilled(ImVec2(min.x + 12.0f, min.y + (frameH * 0.5f)), 5.0f, slotColor(color));
      const float textY = min.y + ((frameH - kRowFont) * 0.5f);
      dl->AddText(ImGui::GetFont(), kRowFont, ImVec2(min.x + 24.0f, textY),
                  on ? ui::kCream : ui::kCreamDim, menu.rowLabel(row).c_str());
      const char* state = on ? "on" : "off";
      dl->AddText(ImGui::GetFont(), kRowFont,
                  ImVec2(max.x - textWidth(kRowFont, state) - 10.0f, textY),
                  on ? ui::kLeafLight : ui::kCreamDim, state);
      ImGui::PopID();
      continue;
    }
    powerUpColumn = 0;
    const float x0 = ImGui::GetCursorPosX();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::vec4(ui::withAlpha(ui::kLeaf, 90)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::vec4(ui::withAlpha(ui::kLeaf, 140)));
    if (ImGui::Selectable("##row", focused, ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0.0f, frameH))) {
      menu.setFocus(i);
    }
    ImGui::PopStyleColor(3);
    if (focused) {
      ui::focusBar(ImVec2(min.x - 2.0f, min.y - 1.0f),
                   ImVec2(min.x + full + 2.0f, min.y + frameH + 1.0f));
    }
    ImGui::SameLine(x0 + 8.0f);
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
  ui::shadowText(ImGui::GetWindowDrawList(), ui::kFontTitle, at, kBanana, share.code.c_str());
  ImGui::Dummy(ImVec2(textWidth(ui::kFontTitle, share.code.c_str()), ImGui::GetFontSize()));
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 18.0f);
  // Without a page URL (natively, unless the build bakes one) the link is
  // the code itself: friends type it into Play online.
  const bool codeOnly = share.link == share.code;
  ImGui::BeginGroup();
  if (ui::bigButton(codeOnly ? "Copy code" : "Copy link", false, 0.0f, true, ui::kFontUi + 2.0f)) {
    out.copy = true;
  }
  if (share.canShare) {
    ImGui::SameLine();
    if (ui::bigButton("Share", false, 0.0f, true, ui::kFontUi + 2.0f)) out.share = true;
  }
  if (share.copied) {
    ImGui::TextColored(ui::vec4(ui::kLeafLight), "Copied: %s", share.link.c_str());
  } else if (codeOnly) {
    ImGui::TextDisabled("Friends enter it in Play online > Room code");
  } else {
    ImGui::TextDisabled("%s", share.link.c_str());
  }
  ImGui::EndGroup();
  return out;
}

}  // namespace

std::array<std::string, sim::kMaxPlayers> slotLabels(const HudView& view) {
  std::array<std::string, sim::kMaxPlayers> l;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    l[i] = client::slotLabel(view.names[i], view.owners[i], i);
  return l;
}

HudView hudView(const MatchSession& session) {
  HudView v;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    v.owners[i] = session.owner(PlayerId{i});
    if (v.owners[i] == SlotOwner::Bot) v.bots = static_cast<sim::PlayerMask>(v.bots | (1u << i));
  }
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
  if (session.room())
    v.roomLine = "Room " + session.room()->code + " (" + session.room()->region + ")";
  // v0.42: the room's nicknames, v0.43 its bots by name too; a room shows
  // its own match over (no server-chosen next match).
  const std::optional<net::RoomStateMsg>& room = session.roomState();
  if (room) {
    v.bots = room->bots;
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      const PlayerId slot{i};
      if (sim::maskHas(room->humans, slot) || sim::maskHas(room->bots, slot)) {
        v.names[i] = session.slotName(slot);
      }
    }
  }
  if (session.configKnown() && !room) {
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
    ui::heading(title, failed ? ui::kAlarm : kBanana);
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
      static constexpr std::array<const char*, 2> kButtons = {"Reconnect", "Leave"};
      buttons.setCount(kButtons.size());
      const float gap = ImGui::GetStyle().ItemSpacing.x;
      const float bw = (ImGui::GetContentRegionAvail().x - gap) / 2.0f;
      for (std::size_t i = 0; i < kButtons.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        if (ui::bigButton(kButtons[i], i == buttons.focus(), bw)) {
          buttons.setFocus(i);
          (i == 0 ? out.reconnect : out.leave) = true;
        }
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
  if (ui::beginScreen("##local_lobby", 900.0f, 0.9f)) {
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
        "Join: press Throw or Dash on a free keyboard side, or any button on a pad");
    ImGui::BulletText("Keyboard left:  WASD move, J/Space throw, K/LShift dash, L/E ability");
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
    if (ui::bigButton("Start match  (Enter / Start)", false, -1.0f, canStart)) out.start = true;
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

    // The roster: the humans by name (the host's crown, you), the bots the
    // host asked for in the lowest open slots (where the server puts them
    // at the start), then the open slots.
    std::array<LobbyCard, sim::kMaxPlayers> cards{};
    const net::RoomSettings& r = room->settings;
    uint8_t botsToCome = r.bots;
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      const PlayerId slot{i};
      LobbyCard& c = cards[i];
      const bool human = sim::maskHas(room->humans, slot);
      const bool bot = sim::maskHas(room->bots, slot);
      c.filled = human || bot;
      if (human) {
        c.title = session.slotName(slot);
        c.host = room->host == slot;
        const bool you = sim::maskHas(session.localSlots(), slot);
        const char* role = c.host ? "host" : "player";
        if (you) role = c.host ? "you, host" : "you";
        std::snprintf(c.sub, sizeof c.sub, "%s", role);
      } else if (bot) {
        c.title = session.slotName(slot);
        std::snprintf(c.sub, sizeof c.sub, "bot");
      } else if (room->bots == 0 && botsToCome > 0) {
        --botsToCome;
        c.filled = true;
        c.alpha = 150;
        c.title = "BOT" + std::to_string(i + 1);
        std::snprintf(c.sub, sizeof c.sub, "joins at the start");
      } else {
        c.title = "open";
        std::snprintf(c.sub, sizeof c.sub, "send the link");
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
      if (ui::bigButton(
              "Start match  (Enter)", true,
              ImGui::GetContentRegionAvail().x - leaveW - ImGui::GetStyle().ItemSpacing.x)) {
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
      ImGui::TextColored(ui::vec4(kBanana), "%s%s", line.c_str(), kDots[dots]);
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

MatchOverView matchOverView(const HudView& view, const presentation::RenderState& state) {
  MatchOverView v;
  const presentation::RenderHud& hud = state.hud;
  v.board = buildScoreboard(hud, slotLabels(view), onlineLocalMask(view), view.bots);
  v.title = "Draw!";
  v.color = kWhite;
  if (hud.matchWinner >= 0 && std::cmp_less(hud.matchWinner, sim::kMaxPlayers)) {
    const auto w = static_cast<uint8_t>(hud.matchWinner);
    char label[kLabelBytes];
    ImU32 color = kWhite;
    winnerText(view, hud, w, label, color);
    v.title = std::string(label) + " wins the match!";
    v.color = color;
  }
  return v;
}

std::optional<std::size_t> drawMatchOver(const MatchOverView& view, ButtonMenu& menu) {
  std::optional<std::size_t> clicked;
  menu.setCount(view.buttons.size());
  if (ui::beginScreen("##match_over", 640.0f)) {
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Match over");
    ImGui::PopFont();
    ui::heading(view.title.c_str(), view.color, ui::kFontHeading + 6.0f);
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = drawScoreTable(ImGui::GetWindowDrawList(), at, width, view.board, false);
    ImGui::Dummy(ImVec2(width, h));
    if (!view.waiting.empty()) {
      ImGui::PushFont(nullptr, ui::kFontButton);
      ImGui::TextColored(ui::vec4(kBanana), "%s", view.waiting.c_str());
      ImGui::PopFont();
    }
    const std::size_t n = view.buttons.size();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float bw =
        n == 0 ? 0.0f : (width - (static_cast<float>(n - 1) * gap)) / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) {
      if (i > 0) ImGui::SameLine();
      if (ui::bigButton(view.buttons[i].c_str(), i == menu.focus(), bw)) {
        menu.setFocus(i);
        clicked = i;
      }
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Left/Right: choose. Enter / A: select.");
    ImGui::PopFont();
  }
  ui::endScreen();
  return clicked;
}

std::optional<PauseItem> drawPauseMenu(PauseMenu& menu) {
  std::optional<PauseItem> clicked;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Over the scene and the HUD (both on the background list), under the panel.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display,
                                                IM_COL32(0, 0, 0, 150));
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
      if (ui::bigButton(PauseMenu::label(items[i]), i == menu.focus(), width)) {
        menu.setFocus(i);
        clicked = items[i];
      }
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Up/Down: choose. Enter / A: select. Esc / B: resume.");
    ImGui::PopFont();
  }
  ui::endScreen();
  return clicked;
}

}  // namespace kfm::client
