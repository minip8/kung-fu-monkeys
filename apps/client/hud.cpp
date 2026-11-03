#include "hud.hpp"

#include "kfm/platform/input_sampler.hpp"
#include "kfm/presentation/palette.hpp"

#include <imgui.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace kfm::client {

namespace {

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

void drawScoreBar(ImDrawList* dl, const HudView& session, const presentation::RenderHud& hud,
                  float viewW) {
  constexpr float kSize = 22.0f;
  constexpr float kPipR = 5.0f;
  constexpr float kGap = 26.0f;
  const uint32_t pips = hud.roundsToWin;

  // Measure, then draw centred.
  float total = 0.0f;
  int entries = 0;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    char label[8];
    slotLabel(session, i, label);
    total += textWidth(kSize, label) + 6.0f + (static_cast<float>(pips) * (kPipR * 2.0f + 4.0f));
    ++entries;
  }
  if (entries == 0) return;
  total += kGap * static_cast<float>(entries - 1);

  float x = (viewW - total) * 0.5f;
  const float y = 14.0f;
  dl->AddRectFilled(ImVec2(x - 12.0f, y - 6.0f), ImVec2(x + total + 12.0f, y + kSize + 6.0f),
                    IM_COL32(0, 0, 0, 110), 8.0f);
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if ((hud.presentMask & (1u << i)) == 0) continue;
    char label[8];
    slotLabel(session, i, label);
    const ImU32 col = slotColor(i);
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, kSize, ImVec2(x + 1.0f, y + 1.0f), kShadow, label);
    dl->AddText(font, kSize, ImVec2(x, y), col, label);
    x += textWidth(kSize, label) + 6.0f + kPipR;
    for (uint32_t p = 0; p < pips; ++p) {
      const ImVec2 c{x, y + (kSize * 0.5f)};
      if (p < hud.scores[i]) {
        dl->AddCircleFilled(c, kPipR, col);
      } else {
        dl->AddCircleFilled(c, kPipR, kEmptyPip);
        dl->AddCircle(c, kPipR, col, 0, 1.5f);
      }
      x += (kPipR * 2.0f) + 4.0f;
    }
    x += kGap - kPipR;
  }

  char round[32];
  std::snprintf(round, sizeof round, "Round %u  (first to %u)", static_cast<unsigned>(hud.round),
                static_cast<unsigned>(hud.roundsToWin));
  centredText(dl, 16.0f, ImVec2(viewW * 0.5f, y + kSize + 18.0f), kWhite, round);
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
  }
}

void drawBanners(ImDrawList* dl, const HudView& session, const presentation::RenderHud& hud,
                 ImVec2 view) {
  const ImVec2 centre{view.x * 0.5f, view.y * 0.42f};
  char text[64];
  switch (hud.phase) {
    case HudPhase::RoundStarting: {
      std::snprintf(text, sizeof text, "Round %u", static_cast<unsigned>(hud.round));
      centredText(dl, 40.0f, ImVec2(centre.x, centre.y - 50.0f), kWhite, text);
      const int n = std::max(1, static_cast<int>(std::ceil(hud.countdownSeconds)));
      std::snprintf(text, sizeof text, "%d", n);
      centredText(dl, 96.0f, ImVec2(centre.x, centre.y + 20.0f), kBananaYellow, text);
      break;
    }
    case HudPhase::RoundEnded: {
      if (hud.lastRoundWinner == kNoSlot) {
        centredText(dl, 56.0f, centre, kWhite, "Draw!");
      } else {
        const auto w = static_cast<uint8_t>(hud.lastRoundWinner);
        char label[8];
        slotLabel(session, w, label);
        std::snprintf(text, sizeof text, "Round won by %s", label);
        centredText(dl, 56.0f, centre, slotColor(w), text);
      }
      break;
    }
    case HudPhase::MatchEnded: {
      if (hud.matchWinner != kNoSlot) {
        const auto w = static_cast<uint8_t>(hud.matchWinner);
        char label[8];
        slotLabel(session, w, label);
        std::snprintf(text, sizeof text, "%s wins the match!", label);
        centredText(dl, 64.0f, centre, slotColor(w), text);
      }
      centredText(dl, 20.0f, ImVec2(centre.x, centre.y + 60.0f), kWhite,
                  session.network ? "Esc: leave the server" : "Enter / Start: back to the lobby");
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

void deviceText(const platform::DeviceBinding& d, char (&out)[96]) {
  switch (d.kind) {
    case platform::DeviceKind::Keyboard:
      if (d.keyboardProfile == 0) {
        std::snprintf(out, sizeof out, "Keyboard left: WASD, throw J/Space, dash K/LShift");
      } else if (d.keyboardProfile == 1) {
        std::snprintf(out, sizeof out, "Keyboard right: arrows, throw ./RCtrl, dash //RShift");
      } else {
        std::snprintf(out, sizeof out, "Keyboard profile %u",
                      static_cast<unsigned>(d.keyboardProfile));
      }
      return;
    case platform::DeviceKind::Gamepad:
      std::snprintf(out, sizeof out, "Gamepad #%u", static_cast<unsigned>(d.padId));
      return;
    case platform::DeviceKind::None:
      break;
  }
  std::snprintf(out, sizeof out, "-");
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
  return v;
}

void drawMatchHud(const HudView& view, const HudInput& in) {
  if (in.state == nullptr || in.camera == nullptr) return;
  // The background list draws over the 3D scene (rendered before ImGui) but under
  // ImGui windows, so the debug overlay stays readable.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const presentation::RenderHud& hud = in.state->hud;
  drawPlayerTags(dl, view, in);
  drawScoreBar(dl, view, hud, display.x);
  drawBanners(dl, view, hud, display);
  drawNotices(dl, view, display);
}

void drawNetStatus(const NetSession& session) {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, view.x - 32.0f), 0.0f), ImGuiCond_Always);
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
    ImGui::TextDisabled("The server runs the match: no pause, step or time scale online.");
    ImGui::TextDisabled("Esc: %s and return to the local lobby. F1: debug overlay",
                        failed ? "close" : "disconnect");
  }
  ImGui::End();
}

bool drawLobby(MatchSession& session, const std::string& connectUrl) {
  const bool online = !connectUrl.empty();
  bool start = false;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, view.x - 32.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.85f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("Kung Fu Monkeys: lobby", nullptr, kFlags)) {
    ImGui::TextUnformatted("Players");
    ImGui::Separator();
    for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
      const SlotOwner owner = session.owner(PlayerId{i});
      if (owner == SlotOwner::None) continue;
      ImGui::PushID(i);
      ImGui::ColorButton("##c", slotColorVec(i), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
      ImGui::PopID();
      ImGui::SameLine();
      if (owner == SlotOwner::Bot) {
        ImGui::Text("Slot %u  BOT", static_cast<unsigned>(i) + 1U);
      } else {
        char dev[96];
        deviceText(session.sampler().binding(PlayerId{i}), dev);
        ImGui::Text("Slot %u  P%u  %s", static_cast<unsigned>(i) + 1U,
                    static_cast<unsigned>(i) + 1U, dev);
      }
    }
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
      ImGui::TextDisabled("Each joined device gets its own slot; the server picks the slots.");
    } else {
      int bots = session.botCount();
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::InputInt("Bots  (- / = keys)", &bots)) {
        session.setBotCount(
            static_cast<uint8_t>(std::clamp(bots, 0, static_cast<int>(sim::kMaxPlayers))));
      }
      ImGui::Text("Seed 0x%llX", static_cast<unsigned long long>(session.seed()));
    }

    ImGui::Spacing();
    ImGui::TextUnformatted(
        "Join: press Throw or Dash on a free keyboard side, or any button on a pad");
    ImGui::BulletText("Keyboard left:  WASD move, J/Space throw, K/LShift dash");
    ImGui::BulletText("Keyboard right: arrows move, ./RCtrl throw, //RShift dash");
    ImGui::BulletText("Gamepad: left stick move, right stick aim, West/RB throw, South/LB dash");
    ImGui::TextDisabled("Aim follows the move direction on keyboards. F1: debug overlay");

    ImGui::Spacing();
    const bool canStart = online || session.lobbyPresentMask() != 0;
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button(online ? "Connect  (Enter / Start)" : "Start match  (Enter / Start)",
                      ImVec2(-1.0f, 0.0f))) {
      start = true;
    }
    if (!canStart) ImGui::EndDisabled();
    if (!online && std::popcount(static_cast<uint32_t>(session.lobbyPresentMask())) < 2) {
      ImGui::TextDisabled("Rounds only end with at least two monkeys: add a player or a bot.");
    }
  }
  ImGui::End();
  return start;
}

}  // namespace kfm::client
