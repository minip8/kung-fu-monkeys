#include "screens.hpp"

#include "core/client_options.hpp"
#include "ui_style.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace kfm::client {

namespace {

// The mouse moved this frame: hovering may take the focus (a resting
// pointer must not fight the keyboard or a pad).
[[nodiscard]] bool mouseMoved() {
  const ImVec2 d = ImGui::GetIO().MouseDelta;
  return d.x != 0.0f || d.y != 0.0f;
}

// A darker band behind the menus so the title reads over any arena.
void dimBackground() {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImU32 top = ui::withAlpha(ui::kJungle, 150);
  const ImU32 bottom = ui::withAlpha(ui::kJungle, 230);
  dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), view, top, top, bottom, bottom);
}

// One menu row: the focus bar when focused, a click on it sets `clicked`.
// The cursor is left at the row's content line.
void menuRow(const char* id, bool focused, float height, bool& clicked) {
  const ImVec2 min = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::PushID(id);
  clicked = ImGui::InvisibleButton("##row", ImVec2(w, height), ImGuiButtonFlags_AllowOverlap);
  ImGui::PopID();
  if (focused) ui::focusBar(ImVec2(min.x - 6.0f, min.y), ImVec2(min.x + w + 6.0f, min.y + height));
  ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + ((height - ImGui::GetFrameHeight()) * 0.5f)));
}

// "<  value  >" at `x` within the row: arrow buttons around a centred value.
// Returns -1, +1 or 0.
int valueArrows(const char* id, float x, float width, const std::string& value) {
  int direction = 0;
  ImGui::PushID(id);
  ImGui::SameLine(x);
  if (ui::arrowButton("##l", ImGuiDir_Left)) direction = -1;
  ImGui::SameLine();
  const float vx = ImGui::GetCursorPosX();
  const float tw = ImGui::CalcTextSize(value.c_str()).x;
  ImGui::SetCursorPosX(vx + std::max(0.0f, (width - tw) * 0.5f));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(value.c_str());
  ImGui::SameLine(vx + width);
  if (ui::arrowButton("##r", ImGuiDir_Right)) direction = +1;
  ImGui::PopID();
  return direction;
}

void footer(const char* text) {
  ImGui::Spacing();
  ImGui::PushFont(nullptr, ui::kFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kCreamDim));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

}  // namespace

std::optional<MainMenuItem> drawMainMenu(MainMenu& menu, const std::string& corner) {
  std::optional<MainMenuItem> chosen;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  dimBackground();
  // The design's menu panel: the title in gold on the bark panel, the items
  // under it in their own colours, Play online the loudest.
  const float panelW = std::min(500.0f, view.x - 32.0f);
  if (ui::beginScreen("##main_menu", panelW, 0.52f)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorScreenPos().x;
    const auto centredLine = [&](float size, ImU32 color, const char* text) {
      const float tw = ui::textWidth(size, text);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ui::shadowText(dl, size, ImVec2(std::round(x0 + ((inner - tw) * 0.5f)), at.y), color, text);
      ImGui::Dummy(ImVec2(inner, size + 2.0f));
    };
    const float titleSize = std::min(ui::kFontTitle - 12.0f, inner / 6.6f);
    centredLine(titleSize, ui::kTitle, "Kung Fu Monkeys");
    centredLine(ui::kFontSmall + 1.0f, ui::kCream,
                "Throw bananas. They come back. Dash to catch them.");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    const auto items = menu.items();
    std::optional<MainMenuItem> pointed;  // a disabled item under the mouse: say why
    for (std::size_t i = 0; i < items.size(); ++i) {
      const MainMenuItem item = items[i];
      const bool enabled = menu.enabled(item);
      ui::Tone tone = ui::Tone::Plain;
      float size = ui::kFontButton;
      if (item == MainMenuItem::PlayOnline) {
        tone = ui::Tone::Lead;
        size = ui::kFontButton + 6.0f;  // the loudest button: larger too
      } else if (item == MainMenuItem::PlayLocal) {
        tone = ui::Tone::Second;
      }
      if (ui::bigButton(MainMenu::label(item), i == menu.focus(), -1.0f, enabled, size, tone)) {
        menu.setFocus(i);
        chosen = item;
      }
      if (ImGui::IsItemHovered()) {
        if (enabled && mouseMoved()) menu.setFocus(i);
        if (!enabled) pointed = item;
      }
    }
    const char* hint = menu.hint(pointed.value_or(menu.focusedItem()));
    ImGui::PushFont(nullptr, ui::kFontSmall);
    const float hw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(
        std::max(ImGui::GetStyle().WindowPadding.x, (ImGui::GetWindowWidth() - hw) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kCreamDim));
    ImGui::TextWrapped("%s", hint);
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
  ui::endScreen();

  ImDrawList* fg = ImGui::GetForegroundDrawList();
  ui::shadowText(fg, ui::kFontSmall, ImVec2(12.0f, view.y - 26.0f), ui::kCreamDim,
                 "Arrows / WASD / pad: choose   Enter / A: select   Esc: back   F1: debug");
  const float cw = ui::textWidth(ui::kFontSmall, corner.c_str());
  ui::shadowText(fg, ui::kFontSmall, ImVec2(view.x - cw - 12.0f, view.y - 26.0f), ui::kCreamDim,
                 corner.c_str());
  return chosen;
}

SettingsOutcome drawSettingsScreen(SettingsMenu& menu, LobbySettings& settings, std::string& nick,
                                   bool& editNick) {
  SettingsOutcome out;
  dimBackground();
  static std::array<char, 17> nickBuf{};  // 16 bytes and the NUL: the wire's limit
  static bool nickEditing = false;
  if (ui::beginScreen("##settings", 600.0f)) {
    ui::heading("Settings");
    ImGui::Spacing();
    constexpr float kValueX = 230.0f;
    constexpr float kValueW = 200.0f;
    const float rowH = ImGui::GetFrameHeight() + 14.0f;
    const auto rows = SettingsMenu::rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const SettingsRow row = rows[i];
      if (row == SettingsRow::Back) continue;
      bool clicked = false;
      menuRow(SettingsMenu::rowLabel(row), i == menu.focus(), rowH, clicked);
      if (clicked) menu.setFocus(i);
      const float x0 = ImGui::GetCursorPosX();
      ImGui::SetCursorPosX(x0 + 8.0f);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(SettingsMenu::rowLabel(row));
      if (row == SettingsRow::Name) {
        if (!nickEditing) {
          const std::size_t n = std::min(nick.size(), nickBuf.size() - 1);
          std::memcpy(nickBuf.data(), nick.data(), n);
          nickBuf[n] = '\0';
        }
        ImGui::SameLine(x0 + kValueX);
        ImGui::SetNextItemWidth(kValueW + 2.0f * ImGui::GetFrameHeight());
        if (editNick) {
          ImGui::SetKeyboardFocusHere();
          editNick = false;
        }
        ui::pushFieldStyle();
        const bool enter = ImGui::InputText(
            "##nick", nickBuf.data(), nickBuf.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ui::popFieldStyle();
        if (ImGui::IsItemActivated()) menu.setFocus(i);
        nickEditing = ImGui::IsItemActive();
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
          out.nickChanged = SettingsMenu::setNick(nick, nickBuf.data()) || out.nickChanged;
          nickEditing = false;
        }
      } else {
        const int d = valueArrows(SettingsMenu::rowLabel(row), x0 + kValueX, kValueW,
                                  SettingsMenu::rowValue(row, settings, nick));
        if (d != 0) {
          menu.setFocus(i);
          out.changed = SettingsMenu::change(i, d, settings) || out.changed;
        }
      }
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
    }
    ImGui::Spacing();
    const std::size_t back = rows.size() - 1;
    if (ui::bigButton("Back", menu.focus() == back, -1.0f)) out.back = true;
    footer(
        "Up/Down: choose. Left/Right: change. Enter on Name: type a new one (16 bytes at most), "
        "Enter to keep it. Esc: back. Your name is what friends see online.");
  }
  ui::endScreen();
  return out;
}

OnlineOutcome drawOnlineScreen(OnlineMenu& menu, bool& editCode, const std::string& lobby) {
  OnlineOutcome out;
  dimBackground();
  static std::array<char, 8> codeBuf{};  // 5 characters and the NUL, with room to spare
  static bool codeEditing = false;
  const auto at = [](OnlineRow r) { return static_cast<std::size_t>(r); };
  if (ui::beginScreen("##online", 600.0f)) {
    ui::heading("Play online");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kCreamDim));
    ImGui::TextWrapped(
        "Make a room and send its link or code to your friends, or join theirs. The room's "
        "host picks the arena and the mode and starts the match.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const float rowH = ImGui::GetFrameHeight() + 14.0f;
    bool clicked = false;
    menuRow("region", menu.focusedRow() == OnlineRow::Region, rowH, clicked);
    if (clicked) menu.setFocus(at(OnlineRow::Region));
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Region");
    const int d = valueArrows("region_value", x0 + 230.0f, 200.0f, menu.regionText());
    if (d != 0) {
      menu.setFocus(at(OnlineRow::Region));
      out.regionChanged = menu.change(at(OnlineRow::Region), d);
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);

    const std::string create = "Create room (" + menu.region() + ")";
    if (ui::bigButton(create.c_str(), menu.focusedRow() == OnlineRow::CreateRoom, -1.0f, true,
                      ui::kFontButton, ui::Tone::Lead)) {
      (void)menu.activate(at(OnlineRow::CreateRoom));
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // The code field and Join on one line.
    if (!codeEditing) {
      const std::string& code = menu.code();
      const std::size_t n = std::min(code.size(), codeBuf.size() - 1);
      std::memcpy(codeBuf.data(), code.data(), n);
      codeBuf[n] = '\0';
    }
    // The label centred on the field's taller line.
    const float lineY = ImGui::GetCursorPosY();
    // The field as tall as the Join button's face beside it.
    const float bigFrame = ui::bigButtonHeight(ui::kFontButton);
    ImGui::SetCursorPosY(lineY + ((bigFrame - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Room code");
    ImGui::SameLine();
    ImGui::SetCursorPosY(lineY);
    const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
    ImGui::PushFont(nullptr, ui::kFontButton);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(12.0f, (bigFrame - ui::kFontButton) * 0.5f));
    ImGui::SetNextItemWidth(150.0f);
    if (editCode) {
      ImGui::SetKeyboardFocusHere();
      editCode = false;
    }
    const auto filter = [](ImGuiInputTextCallbackData* data) -> int {
      return data->EventChar >= 0x80 || !isRoomCodeChar(static_cast<char>(data->EventChar)) ? 1 : 0;
    };
    ui::pushFieldStyle();
    const bool enter = ImGui::InputTextWithHint("##room_code", "ABCDE", codeBuf.data(), 6,
                                                ImGuiInputTextFlags_CharsUppercase |
                                                    ImGuiInputTextFlags_CallbackCharFilter |
                                                    ImGuiInputTextFlags_EnterReturnsTrue,
                                                filter);
    ui::popFieldStyle();
    ImGui::PopStyleVar();
    const ImVec2 fieldMax = ImGui::GetItemRectMax();
    ImGui::PopFont();
    if (ImGui::IsItemActivated()) menu.setFocus(at(OnlineRow::Code));
    codeEditing = ImGui::IsItemActive();
    if (ImGui::IsItemEdited()) menu.setCode(codeBuf.data());
    if (menu.focusedRow() == OnlineRow::Code) {
      ui::focusRing(fieldMin, fieldMax, ImGui::GetStyle().FrameRounding);
    }
    if (enter) {
      menu.setCode(codeBuf.data());
      codeEditing = false;
      if (menu.codeValid()) (void)menu.activate(at(OnlineRow::Join));
    }
    ImGui::SameLine();
    if (ui::bigButton("Join", menu.focusedRow() == OnlineRow::Join, -1.0f, menu.codeValid(),
                      ui::kFontButton, ui::Tone::Second)) {
      (void)menu.activate(at(OnlineRow::Join));
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (ui::bigButton("Back", menu.focusedRow() == OnlineRow::Back, -1.0f)) out.back = true;
    const std::string where = "Lobby " + lobby +
                              ". Left/Right: region (\"no server\": every server there is busy "
                              "right now). Enter on Room code: type it. Esc: back.";
    footer(where.c_str());
  }
  ui::endScreen();
  return out;
}

}  // namespace kfm::client
