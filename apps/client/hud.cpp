#include "hud.hpp"

#include "core/content_labels.hpp"
#include "kfm/content/app_content.hpp"
#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/event_fx.hpp"
#include "kfm/presentation/palette.hpp"
#include "screens.hpp"
#include "ui_style.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cfloat>
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
using ui::Face;
using ui::kBanana;
using ui::kEmptyPip;
using ui::kWhite;
using ui::slotColor;
using ui::textWidth;

// v0.32: a team is drawn in the palette colour of its index (team 1 red,
// team 2 blue, ...), named "Team 1".. on screen.
[[nodiscard]] ImU32 teamColor(uint32_t team, int alpha = 255) {
  return slotColor(team, alpha);
}

// v0.93: a white pill over the game with a colour dot before cocoa text
// (team scores, name tags): a slot colour is not a text colour on sand.
// Returns the pill's width.
float dotPill(ImDrawList* dl, float size, ImVec2 anchor, ImU32 dot, const char* text,
              int alpha = 255) {
  ImFont* f = ui::font(Face::Strong);
  const ImVec2 extent = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float dotR = std::round(size * 0.32f);
  const float padX = std::round(size * 0.45f);
  const float padY = std::round(size * 0.14f);
  const float w = padX + (2.0f * dotR) + std::round(size * 0.3f) + extent.x + padX;
  const ImVec2 lo{std::round(anchor.x - (w * 0.5f)),
                  std::round(anchor.y - (extent.y * 0.5f) - padY)};
  const ImVec2 hi{lo.x + w, std::round(anchor.y + (extent.y * 0.5f) + padY)};
  const float r = (hi.y - lo.y) * 0.5f;
  if (alpha >= 255) ui::softShadow(dl, lo, hi, r, 2.0f, 6.0f, 40);
  dl->AddRectFilled(lo, hi, ui::withAlpha(ui::kHudPill, (230 * alpha) / 255), r);
  const ImVec2 c{lo.x + padX + dotR, (lo.y + hi.y) * 0.5f};
  dl->AddCircleFilled(c, dotR, ui::withAlpha(dot, alpha));
  dl->AddCircle(c, dotR, ui::withAlpha(ui::kCocoa, (60 * alpha) / 255), 0, 1.0f);
  dl->AddText(f, size, ImVec2(c.x + dotR + std::round(size * 0.3f), lo.y + padY),
              ui::withAlpha(ui::kText, alpha), text);
  return w;
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
  ui::pillText(dl, 16.0f, ImVec2(viewW * 0.5f, y), ui::kText, modeLineText(hud).c_str());
  // v0.80: control's zones at a glance (empty in every other kind).
  const ZoneStrip zones = zoneStrip(state);
  if (hud.teamCount > 0) {
    // v0.32: the team scores, each in its team's colour, side by side;
    // control adds the zones each team owns, harvest says "banked".
    y += 28.0f;
    const bool wide = zones.count > 0 || hud.mode == sim::ModeKind::Harvest;
    const float teamW = wide ? 186.0f : 138.0f;
    const uint8_t teams = std::min<uint8_t>(hud.teamCount, sim::kMaxPlayers);
    float x = (viewW * 0.5f) - (static_cast<float>(teams - 1) * teamW * 0.5f);
    for (uint8_t t = 0; t < teams; ++t) {
      const std::string text = teamScoreText(
          hud, t, zones.count > 0 ? std::optional<uint8_t>(zones.owned[t]) : std::nullopt);
      (void)dotPill(dl, 17.0f, ImVec2(x, y), teamColor(t), text.c_str());
      x += teamW;
    }
  }
  if (hud.hasClock) {
    char clock[16];
    clockText(hud.clockSeconds, clock);
    const bool urgent = clockUrgent(hud);
    const float pop = presentation::bannerScale(state, EffectKind::TimerBanner);
    ui::pillText(dl, 24.0f * pop, ImVec2(viewW * 0.5f, y + 30.0f), urgent ? ui::kAlarm : ui::kText,
                 clock, Face::Display);
    y += 30.0f;
  }
  if (zones.count == 0) return;
  // A chip per zone: its owner's colour (neutral when unowned), its letter,
  // and under it the capture under way in the challenger's colour.
  constexpr float kChip = 24.0f;
  constexpr float kGap = 8.0f;
  constexpr float kBarH = 4.0f;
  const float rowW =
      (static_cast<float>(zones.count) * kChip) + (static_cast<float>(zones.count - 1) * kGap);
  float x = (viewW - rowW) * 0.5f;
  const float top = y + 26.0f;
  for (uint8_t k = 0; k < zones.count; ++k) {
    const ZoneChip& chip = zones.chips[k];
    const ImVec2 a{x, top};
    const ImVec2 b{x + kChip, top + kChip};
    ui::softShadow(dl, a, b, 7.0f, 2.0f, 6.0f, 40);
    dl->AddRectFilled(a, b, chip.owner ? teamColor(*chip.owner) : ui::kHudPill, 7.0f);
    dl->AddRect(a, b, kWhite, 7.0f, 0, 2.0f);
    const char letter[2] = {chip.letter, '\0'};
    centredText(dl, 16.0f, ImVec2(x + (kChip * 0.5f), top + (kChip * 0.5f)), kWhite, letter);
    if (chip.challenger) {
      const ImVec2 barA{x, b.y + 3.0f};
      dl->AddRectFilled(barA, ImVec2(b.x, barA.y + kBarH), kEmptyPip);
      dl->AddRectFilled(barA, ImVec2(x + (kChip * chip.capture), barA.y + kBarH),
                        teamColor(*chip.challenger));
    }
    x += kChip + kGap;
  }
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
  dl->AddCircle(c, r, kWhite, 0, 1.0f);
}

// The card every slot is drawn on, in the lobby and in the HUD (v0.93): a
// small soft card in `fill` (white over the game, cream on a screen's card)
// with the slot's palette colour as a band along the top and, in a team
// mode (`team` >= 0), the team's colour along the bottom. A dimmed card
// (alpha < 255) lies flat, without its shadow.
constexpr float kCardStripe = 6.0f;
constexpr float kCardRounding = ui::kChipRounding;
void drawCardFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, uint8_t slot, int alpha, int team = -1,
                   float k = 1.0f, ImU32 fill = ui::kWhite) {
  const float r = kCardRounding * k;
  ui::card(dl, min, max, fill, r, alpha < 255 ? 0.0f : 0.35f, alpha);
  dl->AddRectFilled(min, ImVec2(max.x, min.y + (kCardStripe * k)), slotColor(slot, alpha), r,
                    ImDrawFlags_RoundCornersTop);
  // A hairline round the card: the white slot's band and a white card on
  // the cream wash keep an edge.
  dl->AddRect(min, max, ui::withAlpha(ui::kLine, alpha), r, 0, 1.0f);
  if (team >= 0) {
    dl->AddRectFilled(ImVec2(min.x, max.y - (4.0f * k)), max,
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
  float labelW = textWidth(kLabel, "BOT10", Face::Strong);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) != 0 && !view.names[i].empty()) {
      labelW = std::max(labelW, textWidth(kLabel, view.names[i].c_str(), Face::Strong));
    }
  }
  const float scoreW = frags ? textWidth(kLabel, "-00 (0)", Face::Strong)
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
  ImFont* font = ui::font(Face::Strong);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    const bool waiting = hud.respawnSeconds[i] > 0.0f;
    // Dimmed while out of play: waiting to respawn, or eliminated this round.
    const bool out =
        waiting || (hud.phase == HudPhase::Playing && playerOfSlot(state, i) == nullptr);
    const int alpha = out ? 140 : 255;
    // v0.32: the slot's team as a bar in the team colour along the bottom.
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + h), i, alpha,
                  hud.teamCount > 0 ? static_cast<int>(hud.teams[i]) : -1, k);
    const float lineY = y + ((kCardStripe + 4.0f) * k);
    char label[kLabelBytes];
    slotLabel(view, i, label);
    dl->AddText(font, kLabel * k, ImVec2(x + (kPad * k), lineY), ui::withAlpha(ui::kText, alpha),
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
      const float sw = textWidth(kLabel * k, score, Face::Strong);
      dl->AddText(font, kLabel * k, ImVec2(right - sw, lineY), ui::withAlpha(ui::kText, alpha),
                  score);
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
        dl->AddText(font, 12.0f * k, ImVec2(tx + ((kPieR + 3.0f) * k), ty - (7.0f * k)), ui::kText,
                    secs);
        tx += kTimerStep * k;
      }
    }
    x += w + (kGap * k);
  }
  drawModeLine(dl, state, viewW, y + h + 18.0f);
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
    (void)dotPill(dl, 13.0f, ImVec2(at.x, at.y - 17.0f), slotColor(p.slot), label);

    constexpr float kPipR = 4.0f;
    const float width = static_cast<float>(p.ammoMax) * (kPipR * 2.0f + 3.0f) - 3.0f;
    float x = at.x - (width * 0.5f) + kPipR;
    for (uint8_t a = 0; a < p.ammoMax; ++a) {
      const ImVec2 c{x, at.y};
      if (a < p.ammo) {
        dl->AddCircleFilled(c, kPipR + 1.0f, ui::kBananaInk);
        dl->AddCircleFilled(c, kPipR, kBanana);
      } else {
        dl->AddCircleFilled(c, kPipR + 1.0f, ui::withAlpha(kWhite, 200));
        dl->AddCircleFilled(c, kPipR, kEmptyPip);
      }
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
             ui::kPanelRounding * k);
  }
  ImFont* font = ui::font(Face::Body);
  ImFont* display = ui::font(Face::Display);
  const float left = origin.x + kPad;
  const float right = origin.x + width - kPad;
  const float deathsX = right;
  const float scoreX = b.showDeaths ? right - 90.0f : right;
  const auto rightText = [&](float size, float xRight, float y, ImU32 color, const char* text) {
    dl->AddText(font, size, ImVec2(xRight - textWidth(size, text, Face::Body), y), color, text);
  };
  const auto rightNumber = [&](float size, float xRight, float y, ImU32 color, const char* text) {
    dl->AddText(display, size, ImVec2(xRight - textWidth(size, text), y), color, text);
  };
  float y = origin.y + kPad;
  dl->AddText(font, kSmall, ImVec2(left, y), ui::kTextDim, "#");
  dl->AddText(font, kSmall, ImVec2(left + (52.0f * k), y), ui::kTextDim, "Player");
  rightText(kSmall, scoreX, y, ui::kTextDim, b.scoreLabel);
  if (b.showDeaths) rightText(kSmall, deathsX, y, ui::kTextDim, "Deaths");
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
      dl->AddRectFilled(ImVec2(left - 6.0f, y + 2.0f), ImVec2(right + 6.0f, y + kRowH - 2.0f),
                        slotColor(team, 55), 10.0f * k);
      dl->AddText(display, kText, ImVec2(left, ty), ui::kText, text);
      std::snprintf(text, sizeof text, "%d",
                    static_cast<int>(b.teamScores[team < sim::kMaxPlayers ? team : 0]));
      rightNumber(kText, scoreX, ty, ui::kText, text);
      y += kRowH;
    }
    const float ty = y + ((kRowH - kText) * 0.5f);
    if (r.local) {
      // This client's own rows on a sand band.
      dl->AddRectFilled(ImVec2(left - 6.0f, y + 2.0f), ImVec2(right + 6.0f, y + kRowH - 2.0f),
                        ui::kSandLight, 10.0f * k);
    }
    std::snprintf(text, sizeof text, "%u.", static_cast<unsigned>(r.place));
    dl->AddText(font, kText, ImVec2(left, ty), ui::kTextDim, text);
    // The slot's colour as a dot: the label stays cocoa for contrast.
    dl->AddCircleFilled(ImVec2(left + (40.0f * k), y + (kRowH * 0.5f)), 7.0f * k,
                        slotColor(r.slot));
    dl->AddCircle(ImVec2(left + (40.0f * k), y + (kRowH * 0.5f)), 7.0f * k,
                  ui::withAlpha(ui::kCocoa, 60), 0, 1.0f);
    float nx = left + (52.0f * k);
    if (r.winner) {
      drawCrown(dl, ImVec2(nx, ty + kText - 3.0f), 16.0f);
      nx += 22.0f;
    }
    dl->AddText(font, kText, ImVec2(nx, ty), ui::kText, r.label.c_str());
    nx += textWidth(kText, r.label.c_str(), Face::Body) + 8.0f;
    if (r.local) dl->AddText(font, kSmall, ImVec2(nx, ty + 3.0f), ui::kTextDim, "you");
    std::snprintf(text, sizeof text, "%d", static_cast<int>(r.score));
    rightNumber(kText, scoreX, ty, ui::kText, text);
    if (b.showDeaths) {
      std::snprintf(text, sizeof text, "%u", static_cast<unsigned>(r.deaths));
      rightText(kText, deathsX, ty, ui::kTextDim, text);
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
      centredText(dl, 40.0f * pop, ImVec2(centre.x, centre.y - 50.0f), kWhite,
                  roundStartTitle(hud).c_str());
      const int n = std::max(1, static_cast<int>(std::ceil(hud.countdownSeconds)));
      std::snprintf(text, sizeof text, "%d", n);
      centredText(dl, 96.0f, ImVec2(centre.x, centre.y + 20.0f), kBanana, text);
      // v0.94: the controls in one line, the first round only.
      if (!session.hint.empty() && showsFirstRoundHint(hud)) {
        ui::pillText(dl, 16.0f, ImVec2(centre.x, centre.y + 104.0f), ui::kText,
                     session.hint.c_str());
      }
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
      ui::pillText(dl, 18.0f, ImVec2(centre.x, centre.y + 60.0f), ui::kText, hint);
      if (!session.nextMatch.empty()) {
        ui::pillText(dl, 17.0f, ImVec2(centre.x, centre.y + 92.0f), ui::kTeal,
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
    (void)dotPill(dl, 16.0f, ImVec2(view2.x * 0.5f, y), slotColor(i), text);
    y -= 30.0f;
  }
  if (view.paused) {
    ui::pillText(dl, 26.0f, ImVec2(view2.x * 0.5f, view2.y * 0.2f), ui::kText,
                 "PAUSED  (P: resume, O: step)", Face::Display);
  }
  // Left-aligned lines on white pills along the bottom.
  const auto line = [&](float size, float top, ImU32 color, const std::string& text, Face face) {
    if (text.empty()) return;
    const float w = textWidth(size, text.c_str(), face);
    ui::pillText(dl, size, ImVec2(12.0f + (size * 0.6f) + (w * 0.5f), top + (size * 0.5f)), color,
                 text.c_str(), face);
  };
  line(15.0f, view2.y - 26.0f, ui::kTealDeep, view.statusLine, Face::Strong);
  // M6a: the room, large, and how to invite someone into it.
  line(15.0f, view2.y - 52.0f, ui::kTealDeep, view.shareLine, Face::Strong);
  line(22.0f, view2.y - (view.shareLine.empty() ? 60.0f : 86.0f), ui::kText, view.roomLine,
       Face::Display);
}

// Short device names for the lobby cards.
void shortDeviceText(const platform::DeviceBinding& d, char (&out)[24]) {
  switch (d.kind) {
    case platform::DeviceKind::Keyboard:
      if (d.keyboardProfile == 0) {
        std::snprintf(out, sizeof out, "Keys + mouse");  // v0.45; v0.97 fits a card
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
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ui::kBananaInk, ImDrawFlags_Closed,
                  1.5f);
}

// `perRow`: 0 picks ten in a row, or two rows of five when the window is
// narrow; the online room asks for five (wider cards for nicknames).
// `count`: the first `count` cards only (v8: the online room's slots below
// the server's player cap), at the width and place they have among ten,
// with as many rows as they fill.
void drawLobbyCards(const std::array<LobbyCard, sim::kMaxPlayers>& cards, int perRowWanted = 0,
                    uint8_t count = static_cast<uint8_t>(sim::kMaxPlayers)) {
  constexpr float kGap = 7.0f;
  constexpr float kH = 48.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  int perRow = perRowWanted;
  if (perRow == 0) perRow = avail >= (10.0f * 64.0f) + (9.0f * kGap) ? 10 : 5;
  const float w = (avail - (static_cast<float>(perRow - 1) * kGap)) / static_cast<float>(perRow);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const uint8_t shown = std::min(count, static_cast<uint8_t>(sim::kMaxPlayers));
  const int rows = (static_cast<int>(shown) + perRow - 1) / perRow;
  ImGui::Dummy(ImVec2(avail, (static_cast<float>(rows) * (kH + kGap)) - kGap));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ui::font(Face::Strong);
  ImFont* body = ui::font(Face::Body);
  for (uint8_t i = 0; i < shown; ++i) {
    const LobbyCard& c = cards[i];
    const int column = i % perRow;
    const int row = i / perRow;
    const float x = origin.x + (static_cast<float>(column) * (w + kGap));
    const float y = origin.y + (static_cast<float>(row) * (kH + kGap));
    const int alpha = c.filled ? c.alpha : 110;
    drawCardFrame(dl, ImVec2(x, y), ImVec2(x + w, y + kH), i, alpha, c.team, 1.0f, ui::kInset);
    const ImU32 titleColor = c.filled ? ui::kText : ui::kTextDim;
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w - 2.0f, y + kH), true);
    const float crownW = c.host ? 18.0f : 0.0f;
    if (c.host) drawCrown(dl, ImVec2(x + 7.0f, y + kCardStripe + 17.0f), crownW - 4.0f);
    dl->AddText(font, 18.0f, ImVec2(x + 8.0f + crownW, y + kCardStripe + 1.0f), titleColor,
                c.title.c_str());
    dl->AddText(body, 14.0f, ImVec2(x + 8.0f, y + kCardStripe + 21.0f), ui::kTextDim, c.sub);
    dl->PopClipRect();
    if (c.team >= 0) {
      // v0.32: "T1".. at the right of the title and a bar in the team colour.
      const auto team = static_cast<uint32_t>(c.team);
      char tag[16];
      std::snprintf(tag, sizeof tag, "T%u", team + 1U);
      const float tw = textWidth(15.0f, tag, Face::Strong);
      const ImVec2 tagLo{x + w - tw - 14.0f, y + kCardStripe + 3.0f};
      dl->AddRectFilled(tagLo, ImVec2(x + w - 4.0f, tagLo.y + 18.0f), teamColor(team, alpha), 9.0f);
      dl->AddText(font, 15.0f, ImVec2(tagLo.x + 5.0f, tagLo.y), ui::kWhite, tag);
    }
  }
}

// A top-down sketch of an arena from its def: floor, walls, pits (with their
// rim), slow patches, spawn points in the slot colours (spawn order) and the
// power-up spawners, filled when the match's power-up set leaves them
// something to spawn and hollow when it switches them off.
// v0.52: the player's arena theme, which the preview draws in (setHudTheme()).
presentation::ThemeChoice gTheme = presentation::ThemeChoice::Beach;
// v0.97: the setup screens' preview stops at this height, so the lobby and
// the room fit 720p at any width.
constexpr float kPreviewMaxH = 150.0f;

void drawArenaPreview(const sim::ContentDB& db, const sim::ArenaDef& arena, uint16_t powerUps,
                      ImVec2 box) {
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(box.x, box.y + 4.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // The arena's look in the player's theme (theme.hpp), as the renderer draws it.
  const presentation::ArenaLook& look =
      presentation::arenaLook(presentation::resolveTheme(gTheme, arena.name.view()));
  // On a small card in the theme's surroundings (the sea round a beach).
  ui::card(dl, origin, ImVec2(origin.x + box.x, origin.y + box.y), toU32(look.background), 16.0f,
           0.3f);
  const float hx = std::max(arena.halfExtents.x.toFloat(), 0.01f);
  const float hy = std::max(arena.halfExtents.y.toFloat(), 0.01f);
  const float scale = std::min((box.x - 14.0f) / (2.0f * hx), (box.y - 14.0f) / (2.0f * hy));
  const ImVec2 c{origin.x + (box.x * 0.5f), origin.y + (box.y * 0.5f)};
  const auto at = [&](float x, float y) { return ImVec2(c.x + (x * scale), c.y - (y * scale)); };
  const auto box2 = [&](Vec2 min, Vec2 max) {
    return std::pair{at(min.x.toFloat(), max.y.toFloat()), at(max.x.toFloat(), min.y.toFloat())};
  };
  dl->AddRectFilled(at(-hx, hy), at(hx, -hy), toU32(look.floor), 6.0f);
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
    dl->AddRectFilled(a, b, toU32(look.wall), 2.0f);
    dl->AddRect(a, b, toU32(look.wallInk ? look.ink : look.wallSide), 2.0f, 0, 1.5f);
  }
  dl->AddRect(at(-hx, hy), at(hx, -hy), toU32(look.rim), 6.0f, 0, 2.5f);
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
    dl->AddCircleFilled(at(p.x.toFloat(), p.y.toFloat()), 4.5f, kWhite);
    dl->AddCircleFilled(at(p.x.toFloat(), p.y.toFloat()), 3.5f, slotColor(i));
  }
  const uint32_t all = (uint32_t{1} << std::min<uint8_t>(db.powerUpCount, sim::kMaxPowerUps)) - 1u;
  for (uint8_t i = 0; i < arena.pickupSpawnerCount; ++i) {
    const sim::PickupSpawner& sp = arena.pickupSpawners[i];
    const ImVec2 q = at(sp.pos.x.toFloat(), sp.pos.y.toFloat());
    constexpr float kR = 5.0f;
    if (sp.isTree()) {
      // v0.82: a tree bears bananas whatever the power-up set.
      dl->AddCircleFilled(q, kR + 1.0f, ui::kPreviewTree);
      dl->AddCircle(q, kR + 1.0f, kWhite, 0, 1.5f);
      dl->AddCircleFilled(q, 2.0f, kBanana);
      continue;
    }
    const uint32_t own = sp.powerUpSet == 0 ? all : (sp.powerUpSet & all);
    const bool live = (own & powerUps) != 0;
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

// The match a config selects, in plain words (v0.94, content_labels.hpp):
// the mode's name and how to win, its numbers (modeSummary), the teams, the
// kit with its abilities, the power-ups the arena's spawners will draw and
// the trees. Shared by the online room and the local preview column.
void matchConfigText(const sim::ContentDB& db, const sim::MatchConfig& config) {
  const sim::ArenaDef& arena = db.arenaAt(config.arena);
  const sim::ModeDef& mode = db.modeAt(config.mode);
  const ModeLabel label = modeLabel(mode.name.view());
  ImGui::PushFont(ui::font(Face::Display), ui::kFontUi + 3.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTeal));
  ImGui::TextWrapped("%s on %s", label.title.c_str(), arenaTitle(arena.name.view()).c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (!label.blurb.empty()) ImGui::TextWrapped("%s", label.blurb.c_str());
  std::string numbers = modeSummary(mode);
  if (!numbers.empty()) numbers[0] = static_cast<char>(std::toupper(numbers[0]));
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
  ImGui::TextWrapped("%s.", numbers.c_str());
  if (mode.teams > 0) {
    ImGui::TextWrapped("%u teams, balanced as players join.", static_cast<unsigned>(mode.teams));
  }
  if (mode.roleCount > 0) {
    // v0.97: a mode that hands kits out by role plays no match kit.
    std::string kits;
    for (uint8_t r = 0; r < mode.roleCount && r < sim::kMaxPlayers; ++r) {
      const std::string k = loadoutTitle(db.loadoutAt(mode.roles[r]).name.view());
      if (kits.contains(k)) continue;
      kits += (kits.empty() ? "" : ", ") + k;
    }
    ImGui::TextWrapped("Kits by place in the team: %s.", kits.c_str());
  } else {
    ImGui::TextWrapped("Kit: %s (%s).",
                       loadoutTitle(db.loadoutAt(config.loadout).name.view()).c_str(),
                       loadoutSummary(db, config.loadout).c_str());
  }
  const ArenaSpawners spawners = arenaSpawners(arena);
  if (spawners.powerUps == 0) {
    ImGui::TextWrapped("No power-ups in this arena.");
  } else {
    const uint16_t drawn = static_cast<uint16_t>(arenaPowerUps(db, arena) & config.powerUps);
    std::string names;
    for (uint8_t i = 0; i < db.powerUpCount && i < sim::kMaxPowerUps; ++i) {
      if ((drawn & (1u << i)) == 0) continue;
      if (!names.empty()) names += ", ";
      names += powerUpTitle(db.powerUpAt(i).name.view());
    }
    ImGui::TextWrapped("Power-ups: %s.", names.empty() ? "all switched off" : names.c_str());
  }
  // v0.82: the trees, whatever the power-up set.
  if (spawners.trees > 0) ImGui::TextWrapped("%s.", treesText(spawners.trees).c_str());
  ImGui::PopStyleColor();
}

// The game-setup rows (LobbyMenu, v0.97): always the same seven, in the
// same places. The focused row on the focus bar, a click focuses a row, the
// arrow buttons change it like Left/Right; a greyed row (LobbyMenu::rowEnabled)
// keeps its place with flat arrows and its short reason as the value; a click
// on the Power-ups value opens the picker. Under the rows a line that is
// always there: what the last change did besides itself (notice()), why the
// greyed row under the pointer is greyed, or the preset's blurb. `readOnly`
// for a room's guest: the same rows with flat arrows and no focus.
bool drawSettingsRows(LobbyMenu& menu, bool readOnly = false) {
  bool changed = false;
  const auto rows = LobbyMenu::rows();
  constexpr float kRowFont = 17.0f;
  ImGui::PushFont(nullptr, kRowFont);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
  const float full = ImGui::GetContentRegionAvail().x;
  const float frameH = ImGui::GetFrameHeight();
  const float valueX = std::clamp(full * 0.34f, 92.0f, 140.0f);
  const float valueW = std::max(60.0f, full - valueX - (2.0f * frameH) - 24.0f);
  std::string pointedReason;  // a greyed row under the pointer: why
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const LobbyRowKind row = rows[i];
    const bool enabled = menu.rowEnabled(row);
    const bool live = enabled && !readOnly;
    const bool focused = live && i == menu.focus();
    ImGui::PushID(static_cast<int>(i));
    const float x0 = ImGui::GetCursorPosX();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##row", ImVec2(full, frameH), ImGuiButtonFlags_AllowOverlap) &&
        live) {
      menu.setFocus(i);
    }
    const ImVec2 barMin{min.x - 2.0f, min.y - 1.0f};
    const ImVec2 barMax{min.x + full + 2.0f, min.y + frameH + 1.0f};
    const bool over = ui::pointerOverRect(barMin, barMax);
    if (live && ui::pointerTakesFocus(over)) menu.setFocus(i);
    if (!enabled && over) pointedReason = menu.rowReason(row);
    if (live && ui::showsFocus(focused, over)) ui::focusBar(barMin, barMax);
    ImGui::SetCursorScreenPos(min);
    ImGui::SetCursorPosX(x0 + 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(live ? ui::kText : ui::kTextDim));
    ImGui::TextUnformatted(LobbyMenu::rowLabel(row));
    ImGui::PopStyleColor();
    ImGui::SameLine(x0 + valueX);
    if (ui::arrowButton("##l", ImGuiDir_Left, live)) {
      menu.setFocus(i);
      changed = menu.change(i, -1) || changed;
    }
    ImGui::SameLine();
    const std::string value = menu.rowValue(row);
    const float tw = std::min(valueW, ImGui::CalcTextSize(value.c_str()).x);
    const float vx = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(vx + std::max(0.0f, (valueW - tw) * 0.5f));
    {
      // The value, clipped to its slot so a long name never runs into the arrow.
      const ImVec2 vmin = ImGui::GetCursorScreenPos();
      ImU32 color = ui::kText;
      if (!enabled || readOnly) {
        color = ui::kTextDim;
      } else if (row == LobbyRowKind::Game) {
        color = ui::kTeal;  // the preset: the row that sets the others
      }
      ImGui::PushClipRect(vmin, ImVec2(vmin.x + tw + 2.0f, vmin.y + frameH), true);
      ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(color));
      ImGui::TextUnformatted(value.c_str());
      ImGui::PopStyleColor();
      ImGui::PopClipRect();
      if (row == LobbyRowKind::PowerUps && live) {
        // A click on the set opens the picker; a dotted underline says so.
        const float lineY = vmin.y + frameH - 5.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int dashes = static_cast<int>(std::ceil(tw / 5.0f));
        for (int d = 0; d < dashes; ++d) {
          const float x = vmin.x + (5.0f * static_cast<float>(d));
          dl->AddLine(ImVec2(x, lineY), ImVec2(std::min(x + 2.5f, vmin.x + tw), lineY), ui::kTeal,
                      1.5f);
        }
        ImGui::SetCursorScreenPos(vmin);
        if (ImGui::InvisibleButton("##pick", ImVec2(std::max(tw, 1.0f), frameH))) {
          menu.setFocus(i);
          menu.openPicker();
        }
      }
    }
    ImGui::SameLine(vx + valueW);
    if (ui::arrowButton("##r", ImGuiDir_Right, live)) {
      menu.setFocus(i);
      changed = menu.change(i, +1) || changed;
    }
    ImGui::PopID();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  // The line under the rows, two lines high whatever it says.
  std::string line = menu.notice();
  ImU32 color = ui::kTeal;
  if (line.empty() && !pointedReason.empty()) {
    line = pointedReason;
    color = ui::kTextDim;
  }
  if (line.empty()) {
    color = ui::kTextDim;
    if (const std::optional<std::size_t> p = menu.activePreset()) {
      line = std::string(menu.presets()[*p].def->blurb);
    } else {
      line = "Custom: your own mix. Change Game to start again from a preset.";
    }
  }
  ImGui::PushFont(nullptr, ui::kFontSmall);
  const float y0 = ImGui::GetCursorPosY() + 2.0f;
  ImGui::SetCursorPosY(y0);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(color));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + full);
  ImGui::TextUnformatted(line.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  const float used = ImGui::GetCursorPosY() - y0;
  const float room = 2.0f * ImGui::GetTextLineHeightWithSpacing();
  if (used < room) ImGui::Dummy(ImVec2(0.0f, room - used - ImGui::GetStyle().ItemSpacing.y));
  ImGui::PopFont();
  return changed;
}

// v0.97: the power-up picker over the setup panel (PowerUpPicker): a toggle
// per power-up the arena can draw, two to a row (its colour, its name, on or
// off), then All, None and Done. True when the set changed.
bool drawPowerUpPicker(LobbyMenu& menu, const sim::ContentDB& db) {
  PowerUpPicker& picker = menu.picker();
  if (!picker.isOpen()) return false;
  bool changed = false;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (!ui::beginOverlay("##powerups", std::min(600.0f, view.x - 24.0f))) return false;
  ui::heading("Power-ups");
  ui::bodyText(("The spawners on " + arenaTitle(db.arenaAt(picker.arena()).name.view()) +
                " drop the ones switched on, picked at random.")
                   .c_str(),
               ui::kTextDim, ui::kFontSmall);
  ImGui::Spacing();
  const auto items = picker.items();
  constexpr float kFont = 18.0f;
  ImGui::PushFont(nullptr, kFont);
  const float full = ImGui::GetContentRegionAvail().x;
  const float gap = 10.0f;
  const float w = (full - gap) * 0.5f;
  const float h = kFont + 18.0f;
  for (std::size_t i = 0; i < picker.toggleCount(); ++i) {
    const PowerUpPicker::Item& item = items[i];
    if (picker.columnOf(i) > 0) ImGui::SameLine(0.0f, gap);
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##toggle", ImVec2(w, h))) {
      changed = picker.activate(i, menu.settings()) || changed;
    }
    const bool over = ui::pointerOverItem();
    if (ui::pointerTakesFocus(over)) picker.setFocus(i);
    const ImVec2 max{min.x + w, min.y + h};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool on = (menu.settings().powerUps & (1u << item.powerUp)) != 0;
    if (ui::showsFocus(i == picker.focus(), over)) {
      ui::focusBar(min, max);
    } else {
      dl->AddRectFilled(min, max, on ? ui::kFoamPale : ui::kInset, ui::kRowRounding);
    }
    const uint8_t color = db.powerUpAt(item.powerUp).visual.color;
    dl->AddCircleFilled(ImVec2(min.x + 16.0f, min.y + (h * 0.5f)), 6.0f,
                        on ? slotColor(color) : slotColor(color, 90));
    const float textY = min.y + ((h - kFont) * 0.5f);
    const char* state = on ? "on" : "off";
    const float stateW = textWidth(kFont, state, Face::Strong);
    dl->PushClipRect(min, ImVec2(max.x - stateW - 16.0f, max.y), true);
    dl->AddText(ImGui::GetFont(), kFont, ImVec2(min.x + 30.0f, textY),
                on ? ui::kText : ui::kTextDim,
                powerUpTitle(db.powerUpAt(item.powerUp).name.view()).c_str());
    dl->PopClipRect();
    dl->AddText(ui::font(Face::Strong), kFont, ImVec2(max.x - stateW - 12.0f, textY),
                on ? ui::kTeal : ui::kTextDim, state);
    ImGui::PopID();
  }
  ImGui::PopFont();
  ImGui::Spacing();
  ImGui::Spacing();
  const float bw = (full - (2.0f * ImGui::GetStyle().ItemSpacing.x)) / 3.0f;
  for (std::size_t i = picker.toggleCount(); i < items.size(); ++i) {
    if (i > picker.toggleCount()) ImGui::SameLine();
    const PowerUpPicker::ItemKind kind = items[i].kind;
    const bool done = kind == PowerUpPicker::ItemKind::Done;
    if (ui::bigButton(PowerUpPicker::label(kind), i == picker.focus(), bw, true,
                      ui::kFontButton - 4.0f, done ? ui::Tone::Lead : ui::Tone::Plain)) {
      changed = picker.activate(i, menu.settings()) || changed;
    }
    if (ui::pointerTakesFocus()) picker.setFocus(i);
  }
  ImGui::PushFont(nullptr, ui::kFontSmall);
  ImGui::TextDisabled("Arrows: choose. Enter / A: switch on or off. Esc / B: done.");
  ImGui::PopFont();
  ui::endOverlay();
  return changed;
}

// v0.43: the room's code, large, with Copy link and (touch) Share. v0.97:
// `compact`, the waiting room's: one row (the label, the code, the buttons,
// then what the link is or "Copied"), so the room fits 720p with it.
ShareOutcome drawShare(const ShareView& share, bool compact = false) {
  ShareOutcome out;
  if (share.code.empty()) return out;
  const float codeSize = compact ? 44.0f : ui::kFontTitle;
  const float y0 = ImGui::GetCursorPosY();
  ImGui::PushFont(nullptr, ui::kFontUi);
  if (compact) ImGui::SetCursorPosY(y0 + ((codeSize - ImGui::GetFontSize()) * 0.5f));
  ImGui::TextDisabled("Room code (%s)", share.region.c_str());
  ImGui::PopFont();
  if (compact) {
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetCursorPosY(y0);
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushFont(ui::font(Face::Display), codeSize);
  ImGui::GetWindowDrawList()->AddText(ui::font(Face::Display), codeSize, at, ui::kTeal,
                                      share.code.c_str());
  ImGui::Dummy(ImVec2(textWidth(codeSize, share.code.c_str()), ImGui::GetFontSize()));
  ImGui::PopFont();
  ImGui::SameLine(0.0f, 12.0f);
  const float buttonH = ui::bigButtonHeight(ui::kFontUi + 2.0f) + ui::kButtonDrop;
  const float rowTop = compact ? y0 : ImGui::GetCursorPosY();
  ImGui::SetCursorPosY(rowTop + std::max(0.0f, (codeSize - buttonH) * 0.5f));
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
  if (compact) {
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetCursorPosY(y0 + 2.0f);
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ui::kFontSmall);
  }
  ImGui::PushTextWrapPos(0.0f);
  if (share.copied) {
    ImGui::TextColored(ui::vec4(ui::kSuccess), "Copied: %s", share.link.c_str());
  } else if (codeOnly) {
    ImGui::TextDisabled("Friends enter it in Play online > Room code");
  } else {
    ImGui::TextDisabled("%s", share.link.c_str());
  }
  ImGui::PopTextWrapPos();
  if (compact) ImGui::PopFont();
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
  ui::menuBackdrop();
  if (ui::beginScreen("##online_status", 760.0f)) {
    const NetPhase phase = session.phase();
    const bool failed =
        phase == NetPhase::Rejected || phase == NetPhase::Disconnected || phase == NetPhase::Failed;
    const bool lost = session.canReconnect();
    const char* title = session.room() ? "Joining the room" : "Connecting";
    // v0.92: also while the session reconnects on its own.
    if (session.reconnects() > 0 || session.autoReconnecting()) title = "Reconnecting";
    if (failed) title = "Could not join";
    if (lost) title = "Connection lost";
    ui::heading(title, failed ? ui::kAlarm : ui::kTitle);
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
    // match in progress for a minute (v0.92).
    if (lost) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Your place in the match is kept for a minute after a drop.");
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
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    if (session.config().spectate) {
      ImGui::TextUnformatted("You are watching: no monkey of yours in this match.");
    } else {
      // v0.94: in plain words.
      const unsigned players = session.deviceCount();
      ImGui::Text("Playing from this device: %u %s", players, players == 1 ? "monkey" : "monkeys");
    }
    ImGui::PopStyleColor();
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
    // v0.94: a visible way back on every state but the lost connection's
    // (which has its own Leave).
    if (!lost) {
      ImGui::Spacing();
      if (ui::bigButton(failed ? "Back to the menu" : "Cancel", true, -1.0f)) out.leave = true;
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Esc / B: %s and go back to the menu.", failed ? "close" : "disconnect");
    ImGui::PopFont();
  }
  ui::endScreen();
  return out;
}

LobbyOutcome drawLobby(MatchSession& session, LobbyMenu& menu) {
  LobbyOutcome out;
  ui::menuBackdrop();
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##local_lobby", std::min(980.0f, view.x - 24.0f))) {
    ui::heading("Local play");
    // Who plays: the slot cards, and how to join one.
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
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    ImGui::TextWrapped(
        "More players: press Throw or Dash on a free side of the keyboard, or any gamepad "
        "button.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    const sim::PlayerMask dropped = session.droppedMask();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      if ((dropped & (1u << i)) != 0) {
        ImGui::TextColored(ui::vec4(ui::kAlarm), "P%u's controller disconnected",
                           static_cast<unsigned>(i) + 1U);
      }
    }

    ImGui::Spacing();
    const sim::ContentDB& db = session.content();
    const LobbySettings& ls = menu.settings();
    const sim::MatchConfig preview{.seed = session.seed(),
                                   .arena = ls.arena,
                                   .mode = ls.mode,
                                   .powerUps = ls.powerUps,
                                   .loadout = ls.loadout};
    // Two columns from about 560 px of content: the settings, then the
    // arena's preview with the match in words; one column under that.
    const bool twoColumns = ImGui::GetContentRegionAvail().x >= 560.0f;
    if (twoColumns && ImGui::BeginTable("lobby_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("rows", ImGuiTableColumnFlags_WidthStretch, 1.1f);
      ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextColumn();
      out.changed = drawSettingsRows(menu);
      ImGui::TableNextColumn();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
      drawArenaPreview(db, db.arenaAt(ls.arena), ls.powerUps,
                       ImVec2(w, std::min(w * 0.5f, kPreviewMaxH)));
      matchConfigText(db, preview);
      ImGui::PopFont();
      ImGui::EndTable();
    } else if (!twoColumns) {
      out.changed = drawSettingsRows(menu);
      ImGui::Spacing();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      matchConfigText(db, preview);
      ImGui::PopFont();
    }

    ImGui::Spacing();
    // The one next step, and the way back, always on screen.
    const bool canStart = session.lobbyPresentMask() != 0;
    const float backW = ui::bigButtonWidth("Back", ui::kFontButton);
    if (ui::bigButton("Back", false, backW)) out.back = true;
    ImGui::SameLine();
    // v0.97: the player's preferences, over the lobby (Tab / a pad's View).
    if (ui::bigButton("Settings", false, ui::bigButtonWidth("Settings", ui::kFontButton))) {
      out.settings = true;
    }
    ImGui::SameLine();
    if (ui::bigButton("Start match", false, ImGui::GetContentRegionAvail().x, canStart,
                      ui::kFontButton, ui::Tone::Lead)) {
      out.start = true;
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    if (!canStart) {
      ImGui::TextWrapped("Nobody is in yet: join with a key or a pad, or add a bot.");
    } else if (std::popcount(static_cast<uint32_t>(session.lobbyPresentMask())) < 2) {
      ImGui::TextWrapped("A round needs two monkeys to end: add a player or a bot.");
    }
    ImGui::TextWrapped(
        "Enter / Start: start. Arrows: choose and change a setting. Tab / View: settings. "
        "Esc / B: back.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ui::endScreen();
  // v0.97: the power-up picker over the panel.
  out.changed = drawPowerUpPicker(menu, session.content()) || out.changed;
  return out;
}

// --- the online room (v0.42, restyled in v0.43, v0.94) ------------------------------------

RoomOutcome drawRoomScreen(const NetSession& session, LobbyMenu& menu, const ShareView& share) {
  RoomOutcome out;
  const std::optional<net::RoomStateMsg>& room = session.roomState();
  if (!room) return out;
  const bool host = session.isHost();
  const sim::ContentDB& db = session.content();
  ui::menuBackdrop();
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##room", std::min(980.0f, view.x - 24.0f))) {
    // Who hosts, then the room's code and the invite, prominent.
    const std::string hostName =
        room->host.isValid() ? session.slotName(room->host) : std::string("the host");
    ui::heading(host ? "Your room" : (hostName + "'s room").c_str());
    if (!share.code.empty()) out.share = drawShare(share, true);
    ImGui::Spacing();

    // The roster (roomSlots()): the humans by name (the host's crown, you),
    // the bots where the server says it puts them at the start, then the
    // open slots. v8 (issue #83): the slots at or above the server's player
    // cap (Closed, always the highest) get no card: the grid ends at the
    // cap, so nothing past it looks joinable.
    std::array<LobbyCard, sim::kMaxPlayers> cards{};
    const net::RoomSettings& r = room->settings;
    const std::array<RoomSlot, sim::kMaxPlayers> slots = roomSlots(*room);
    uint8_t cardCount = 0;
    uint8_t humans = 0;
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      if (slots[i] == RoomSlot::Closed) continue;
      const PlayerId slot{i};
      LobbyCard& c = cards[i];
      c.filled = slots[i] != RoomSlot::Open;
      switch (slots[i]) {
        case RoomSlot::Human: {
          ++humans;
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
        case RoomSlot::Closed:
          break;  // skipped above
      }
      cardCount = static_cast<uint8_t>(i + 1);
    }
    drawLobbyCards(cards, 5, cardCount);
    ImGui::Spacing();

    // The match: the host changes it, everyone else sees the same rows,
    // flat, with the reason.
    const sim::MatchConfig shown{
        .arena = r.arena, .mode = r.mode, .powerUps = r.powerUps, .loadout = r.loadout};
    const bool twoColumns = ImGui::GetContentRegionAvail().x >= 560.0f;
    const auto rowsColumn = [&] {
      if (!host) {
        ImGui::PushFont(nullptr, ui::kFontSmall);
        ImGui::TextColored(ui::vec4(ui::kTextDim), "Set by %s: only the host changes the match.",
                           hostName.c_str());
        ImGui::PopFont();
      }
      out.settingsChanged = drawSettingsRows(menu, !host) && host;
    };
    if (twoColumns && ImGui::BeginTable("room_match", 2, ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("rows", ImGuiTableColumnFlags_WidthStretch, 1.1f);
      ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextColumn();
      rowsColumn();
      ImGui::TableNextColumn();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
      drawArenaPreview(db, db.arenaAt(r.arena), r.powerUps,
                       ImVec2(w, std::min(w * 0.5f, kPreviewMaxH)));
      matchConfigText(db, shown);
      ImGui::PopFont();
      ImGui::EndTable();
    } else if (!twoColumns) {
      rowsColumn();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      matchConfigText(db, shown);
      ImGui::PopFont();
    }

    ImGui::Spacing();
    const float leaveW = ui::bigButtonWidth("Leave", ui::kFontButton);
    if (ui::bigButton("Leave", false, leaveW)) out.leave = true;
    ImGui::SameLine();
    // v0.97: the player's preferences, over the room (Tab / a pad's View).
    if (ui::bigButton("Settings", false, ui::bigButtonWidth("Settings", ui::kFontButton))) {
      out.openSettings = true;
    }
    ImGui::SameLine();
    if (host) {
      if (ui::bigButton("Start match", true, ImGui::GetContentRegionAvail().x, true,
                        ui::kFontButton, ui::Tone::Lead)) {
        out.start = true;
      }
    } else {
      const std::string line = "Waiting for " + hostName + " to start";
      // v0.97: beside Leave and Settings, smaller when a narrow window needs it.
      const float fits = ImGui::GetContentRegionAvail().x /
                         std::max(1.0f, textWidth(1.0f, (line + "...").c_str()));
      ImGui::PushFont(ui::font(Face::Display), std::clamp(fits, ui::kFontSmall, ui::kFontButton));
      constexpr const char* kDots[] = {"", ".", "..", "..."};
      const auto dots = static_cast<std::size_t>(ImGui::GetTime() * 2.0) % 4;
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                           ((ui::bigButtonHeight() - ImGui::GetFontSize()) * 0.5f));
      ImGui::TextColored(ui::vec4(ui::kTeal), "%s%s", line.c_str(), kDots[dots]);
      ImGui::PopFont();
    }
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    if (host && humans < 2 && r.bots == 0) {
      ImGui::TextWrapped("You are alone: send the link, or add bots, then Start.");
    }
    ImGui::TextWrapped("%s", host ? "Enter: start. Arrows: choose and change a setting. Tab / "
                                    "View: settings. Esc / B: leave the room."
                                  : "Tab / View: settings. Esc / B: leave the room.");
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ui::endScreen();
  // v0.97: the host's power-up picker over the panel.
  if (host && drawPowerUpPicker(menu, db)) out.settingsChanged = true;
  return out;
}

std::optional<MatchOverAction> drawMatchOver(const MatchOverView& view, ButtonMenu& menu) {
  std::optional<MatchOverAction> clicked;
  menu.setCount(view.actions.size());
  if (ui::beginScreen("##match_over", 640.0f)) {
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::TextDisabled("Match over");
    ImGui::PopFont();
    {
      // The winner's title in the winner's colour, as a sticker so every
      // slot colour (white too) reads on the white card.
      const float size = ui::kFontHeading + 6.0f;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ui::shadowText(ImGui::GetWindowDrawList(), size, ImVec2(at.x + 2.0f, at.y),
                     view.colorIndex ? slotColor(*view.colorIndex) : ui::kTitle,
                     view.title.c_str());
      ImGui::Dummy(ImVec2(textWidth(size, view.title.c_str()) + 4.0f, size + 6.0f));
    }
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = drawScoreTable(ImGui::GetWindowDrawList(), at, width, view.board, false);
    ImGui::Dummy(ImVec2(width, h));
    if (!view.waiting.empty()) {
      ImGui::PushFont(ui::font(Face::Display), ui::kFontButton);
      ImGui::TextColored(ui::vec4(ui::kTeal), "%s", view.waiting.c_str());
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
  dl->AddRectFilled(lo, hi, ui::kInset, kTrackH * 0.5f);
  const float knobX = x0 + (trackW * frac);
  if (knobX > lo.x + 2.0f) {
    dl->AddRectFilled(lo, ImVec2(knobX, hi.y), ui::kOcean, kTrackH * 0.5f);
  }
  dl->AddCircleFilled(ImVec2(knobX, cy + 3.0f), kKnobR + 1.0f, ui::kFoamLip);
  ImU32 knob = over ? ui::kFoamPale : ui::kWhite;
  if (ImGui::IsItemActive()) knob = ui::kBanana;
  dl->AddCircleFilled(ImVec2(knobX, cy), kKnobR + 1.0f, ui::kTealDeep);
  dl->AddCircleFilled(ImVec2(knobX, cy), kKnobR - 1.0f, knob);
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
  float copyW{0.0f};  // 0: no Copy link (no page URL)
  float menuW{0.0f};
  float timeW{0.0f};
  ReplayBarRows rows{};
  float titleH{0.0f};
  float height{0.0f};
};

[[nodiscard]] float replayButtonWidth(const char* text) {
  return ui::bigButtonWidth(text, ReplayBarLayout::kSize);
}

[[nodiscard]] ReplayBarLayout replayBarLayout(bool titled, bool copyLink) {
  ReplayBarLayout l;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  l.faceH = ui::bigButtonHeight(ReplayBarLayout::kSize);
  l.rowH = l.faceH + ui::kButtonDrop;
  l.width = std::max(display.x - (2.0f * ReplayBarLayout::kMargin), 1.0f);
  l.restartW = replayButtonWidth("Restart");
  l.playW = std::max(
      {replayButtonWidth("Watch again"), replayButtonWidth("Pause"), replayButtonWidth("Play")});
  l.stepW = replayButtonWidth("Step");
  l.speedW = 0.0f;  // the widest speed label, so the bar does not shift as it cycles
  for (std::size_t i = 0; i < kReplaySpeeds.size(); ++i)
    l.speedW = std::max(l.speedW, replayButtonWidth(replaySpeedLabel(i).c_str()));
  l.copyW = copyLink ? std::max(replayButtonWidth("Copy link"), replayButtonWidth("Copied")) : 0.0f;
  l.menuW = replayButtonWidth("Main menu");
  l.timeW = textWidth(ui::kFontUi, "00:00 / 00:00");
  constexpr float kGap = ReplayBarLayout::kGap;
  l.rows = replayBarRows(
      ReplayBarPieces{.inner = l.width - (2.0f * ReplayBarLayout::kPad),
                      .transport = l.restartW + l.playW + l.stepW + l.speedW + (3.0f * kGap),
                      .time = l.timeW,
                      .copy = l.copyW,
                      .menu = l.menuW,
                      .gap = kGap});
  l.titleH = titled ? ui::kFontSmall + 6.0f : 0.0f;
  l.height = (2.0f * ReplayBarLayout::kPad) + l.titleH + l.rowH +
             (l.rows.twoRows ? l.rowH + ReplayBarLayout::kRowGap : 0.0f);
  return l;
}

}  // namespace

float replayBarInset(bool titled, bool copyLink) {
  return replayBarLayout(titled, copyLink).height + ReplayBarLayout::kMargin + ui::kPanelDrop;
}

ReplayBarOutcome drawReplayBar(ReplayControls& controls, const ReplayBarView& view) {
  ReplayBarOutcome out;
  const ReplayStatus& s = view.status;
  controls.refocus(s);  // Step disabled under the focus: Play / Pause has it
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const ReplayBarLayout layout = replayBarLayout(!view.title.empty(), s.canCopyLink);
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
             ui::kPanelRounding);
  }
  if (open) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!view.title.empty()) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      dl->AddText(ImGui::GetFont(), ui::kFontSmall, at, ui::kTextDim, view.title.c_str());
      ImGui::Dummy(ImVec2(1.0f, titleH - ImGui::GetStyle().ItemSpacing.y));
    }
    const float menuW = layout.menuW;
    // Copy link (when shown, and not moved up beside the time) and Main
    // menu, at the row's end.
    const bool copyBelow = s.canCopyLink && !layout.rows.copyOnTop;
    const float endW = copyBelow ? layout.copyW + ReplayBarLayout::kGap + menuW : menuW;
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
    const auto menuButtons = [&] {
      if (copyBelow) {
        button(ReplayControl::CopyLink, layout.copyW, ui::Tone::Plain);
        ImGui::SameLine();
      }
      button(ReplayControl::MainMenu, menuW, ui::Tone::Plain);
    };
    const auto timeText = [&] {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(timeW, rowH));
      const float tw = textWidth(ui::kFontUi, time.c_str());
      dl->AddText(ui::font(Face::Display), ui::kFontUi,
                  ImVec2(at.x + timeW - tw, at.y + ((faceH - ui::kFontUi) * 0.5f)), ui::kText,
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
      if (layout.rows.copyOnTop) {
        ImGui::SameLine();
        button(ReplayControl::CopyLink, layout.copyW, ui::Tone::Plain);
      }
      transport();
      ImGui::SameLine();
      const float rest = ImGui::GetContentRegionAvail().x;
      if (rest > endW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rest - endW);
      menuButtons();
    } else {
      transport();
      ImGui::SameLine();
      drawReplayTimeline(controls, s, layout.rows.timeline, faceH, out);
      ImGui::SameLine();
      timeText();
      ImGui::SameLine();
      menuButtons();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  return out;
}

std::optional<PauseItem> drawPauseMenu(PauseMenu& menu, const HowToPlay& card) {
  std::optional<PauseItem> clicked;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  // Over the scene and the HUD (both on the background list), under the panel.
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), display, ui::kScrim);
  if (menu.helpOpen()) {
    // v0.94: the controls card in the menu's place; Back returns to it.
    if (ui::beginScreen("##pause_help", std::min(780.0f, display.x - 24.0f))) {
      ui::heading("How to play");
      bool back = false;
      drawHowToPlay(card, true, back);
      if (back) clicked = PauseItem::HowToPlay;
    }
    ui::endScreen();
    return clicked;
  }
  if (ui::beginScreen("##pause_menu", 400.0f)) {
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
