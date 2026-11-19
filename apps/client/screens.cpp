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

// v0.93: a cream wash behind the menus so the cards and the title read over
// any arena.
void dimBackground() {
  ui::menuBackdrop();
}

// One menu row: the focus bar when highlighted (ui::showsFocus: focused in
// keyboard / pad mode, under the pointer in pointer mode). `takesFocus` is
// set by a click on the row and, in pointer mode, by the pointer over it.
// The cursor is left at the row's content line.
void menuRow(const char* id, bool focused, float height, bool& takesFocus) {
  const ImVec2 min = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::PushID(id);
  const bool clicked =
      ImGui::InvisibleButton("##row", ImVec2(w, height), ImGuiButtonFlags_AllowOverlap);
  ImGui::PopID();
  const ImVec2 lo{min.x - 6.0f, min.y};
  const ImVec2 hi{min.x + w + 6.0f, min.y + height};
  const bool over = ui::pointerOverRect(lo, hi);
  takesFocus = ui::pointerTakesFocus(over) || clicked;
  if (ui::showsFocus(focused, over)) ui::focusBar(lo, hi);
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
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

// A small teal heading over a group of rows.
void sectionLabel(const char* text) {
  ImGui::PushFont(ui::font(ui::Face::Display), ui::kFontUi + 3.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTeal));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

}  // namespace

std::optional<MainMenuItem> drawMainMenu(MainMenu& menu, const std::string& corner) {
  std::optional<MainMenuItem> chosen;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  dimBackground();
  const char* hintText = nullptr;
  float cardBottom = 0.0f;
  // v0.93, the style board's menu block: the title in white on the ocean
  // card, the items under it in their own colours, Play online the
  // loudest; the tagline and the focused item's hint on the cream wash.
  const float panelW = std::min(500.0f, view.x - 32.0f);
  if (ui::beginScreen("##main_menu", panelW, 0.5f, ui::kOcean)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float titleSize = std::min(ui::kFontTitle - 12.0f, inner / 6.4f);
    {
      // White Lilita One with a deep-teal lip under it, like the buttons'.
      const char* title = "Kung Fu Monkeys";
      const float tw = ui::textWidth(titleSize, title);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const ImVec2 pos{std::round(x0 + ((inner - tw) * 0.5f)), at.y};
      ImFont* display = ui::font(ui::Face::Display);
      dl->AddText(display, titleSize, ImVec2(pos.x, pos.y + std::round(titleSize * 0.07f)),
                  ui::kTealDeep, title);
      dl->AddText(display, titleSize, pos, ui::kWhite, title);
      ImGui::Dummy(ImVec2(inner, titleSize + 6.0f));
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

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
      // Pointer mode: the item under the pointer takes the focus (a key press
      // then starts there); keyboard / pad mode: a resting pointer does not.
      if (enabled && ui::pointerTakesFocus()) menu.setFocus(i);
      if (!enabled && ImGui::IsItemHovered()) pointed = item;
    }
    hintText = menu.hint(pointed.value_or(menu.focusedItem()));
    cardBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
  }
  ui::endScreen();

  // Under the ocean card, on the wash: the tagline and the focused item's
  // hint (white on the ocean is 3.6:1, too little for small text).
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  ImFont* body = ui::font(ui::Face::Body);
  const auto centred = [&](float size, float y, ImU32 color, const char* text) {
    const float tw = ui::textWidth(size, text, ui::Face::Body);
    fg->AddText(body, size, ImVec2(std::round((view.x - tw) * 0.5f), y), color, text);
  };
  if (cardBottom > 0.0f) {
    centred(ui::kFontUi, cardBottom + 22.0f, ui::kText,
            "Throw bananas. They come back. Dash to catch them.");
    if (hintText != nullptr) centred(ui::kFontSmall, cardBottom + 50.0f, ui::kTextDim, hintText);
  }
  fg->AddText(body, ui::kFontSmall, ImVec2(14.0f, view.y - 28.0f), ui::kTextDim,
              "Arrows / WASD / pad: choose   Enter / A: select   Esc: back   F1: debug");
  const float cw = ui::textWidth(ui::kFontSmall, corner.c_str(), ui::Face::Body);
  fg->AddText(body, ui::kFontSmall, ImVec2(view.x - cw - 14.0f, view.y - 28.0f), ui::kTextDim,
              corner.c_str());
  return chosen;
}

SettingsOutcome drawSettingsOverlay(SettingsMenu& menu, LobbySettings& settings, std::string& nick,
                                    bool& editNick) {
  SettingsOutcome out;
  static std::array<char, 17> nickBuf{};  // 16 bytes and the NUL: the wire's limit
  static bool nickEditing = false;
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (!ui::beginOverlay("##settings", std::min(600.0f, view.x - 24.0f))) return out;
  ui::heading("Settings");
  ui::bodyText("Yours on this device: they apply at once and are kept.", ui::kTextDim,
               ui::kFontSmall);
  ImGui::Spacing();
  const float full = ImGui::GetContentRegionAvail().x;
  const float valueX = std::clamp(full * 0.36f, 120.0f, 220.0f);
  const float valueW = std::max(110.0f, full - valueX - (2.0f * ImGui::GetFrameHeight()) - 16.0f);
  const float rowH = ImGui::GetFrameHeight() + 12.0f;
  const auto rows = SettingsMenu::rows();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const SettingsRow row = rows[i];
    if (row == SettingsRow::Back) continue;
    bool takesFocus = false;
    menuRow(SettingsMenu::rowLabel(row), i == menu.focus(), rowH, takesFocus);
    if (takesFocus) menu.setFocus(i);
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
      ImGui::SameLine(x0 + valueX);
      ImGui::SetNextItemWidth(valueW + (2.0f * ImGui::GetFrameHeight()));
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
      const int d = valueArrows(SettingsMenu::rowLabel(row), x0 + valueX, valueW,
                                SettingsMenu::rowValue(row, settings, nick));
      if (d != 0) {
        menu.setFocus(i);
        out.changed = SettingsMenu::change(i, d, settings) || out.changed;
      }
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
  }
  ImGui::Spacing();
  const std::size_t done = rows.size() - 1;
  if (ui::bigButton("Done", menu.focus() == done, -1.0f, true, ui::kFontButton, ui::Tone::Lead)) {
    out.back = true;
  }
  if (ui::pointerTakesFocus()) menu.setFocus(done);
  footer(
      "Up/Down: choose. Left/Right: change. Enter on Name: type a new one (16 bytes at most), "
      "Enter to keep it. Esc / B: done. Your name is what friends see online.");
  ui::endOverlay();
  return out;
}

OnlineOutcome drawOnlineScreen(OnlineMenu& menu, OnlineFields& fields) {
  OnlineOutcome out;
  dimBackground();
  static std::array<char, 8> codeBuf{};   // 5 characters and the NUL, with room to spare
  static std::array<char, 17> nameBuf{};  // 16 bytes and the NUL: the wire's limit
  static bool codeEditing = false;
  static bool nameEditing = false;
  const auto at = [](OnlineRow r) { return static_cast<std::size_t>(r); };
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##online", std::min(620.0f, view.x - 24.0f))) {
    ui::heading("Play online");

    // v0.94: the name first, editable right here and saved like Settings.
    if (!nameEditing) {
      const std::size_t n = std::min(fields.nick.size(), nameBuf.size() - 1);
      std::memcpy(nameBuf.data(), fields.nick.data(), n);
      nameBuf[n] = '\0';
    }
    const float bigFrame = ui::bigButtonHeight(ui::kFontButton);
    sectionLabel("Your name");
    const ImVec2 nameMin = ImGui::GetCursorScreenPos();
    ImGui::PushFont(ui::font(ui::Face::Display), ui::kFontButton - 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(16.0f, (bigFrame - ui::kFontButton + 4.0f) * 0.5f));
    ImGui::SetNextItemWidth(-1.0f);
    if (fields.editName) {
      ImGui::SetKeyboardFocusHere();
      fields.editName = false;
    }
    ui::pushFieldStyle();
    const bool nameEnter = ImGui::InputTextWithHint(
        "##online_name", "Your monkey name", nameBuf.data(), nameBuf.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ui::popFieldStyle();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    const ImVec2 nameMax = ImGui::GetItemRectMax();
    const bool overName = ui::pointerOverItem();
    if (ui::pointerTakesFocus(overName) || ImGui::IsItemActivated()) {
      menu.setFocus(at(OnlineRow::Name));
    }
    nameEditing = ImGui::IsItemActive();
    if (nameEnter || ImGui::IsItemDeactivatedAfterEdit()) {
      out.nickChanged = SettingsMenu::setNick(fields.nick, nameBuf.data());
      out.nickCommitted = true;
      nameEditing = false;
    }
    if (ui::showsFocus(menu.focusedRow() == OnlineRow::Name, overName) || nameEditing) {
      ui::focusRing(nameMin, nameMax, ui::kRowRounding);
    }
    ui::bodyText(OnlineMenu::nameHint(fields.nickChosen), ui::kTextDim, ui::kFontSmall);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // Start a room: the region, then Create room.
    sectionLabel("Start a room");
    const float rowH = ImGui::GetFrameHeight() + 14.0f;
    bool takesFocus = false;
    menuRow("region", menu.focusedRow() == OnlineRow::Region, rowH, takesFocus);
    if (takesFocus) menu.setFocus(at(OnlineRow::Region));
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Region");
    const float avail = ImGui::GetContentRegionAvail().x;
    const float valueW = std::min(200.0f, std::max(110.0f, avail - 180.0f));
    const float valueX = x0 + std::max(110.0f, avail - valueW - (2.0f * ImGui::GetFrameHeight()));
    const int d = valueArrows("region_value", valueX, valueW, menu.regionText());
    if (d != 0) {
      menu.setFocus(at(OnlineRow::Region));
      out.regionChanged = menu.change(at(OnlineRow::Region), d);
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
    const std::string create = "Create a room (" + menu.region() + ")";
    if (ui::bigButton(create.c_str(), menu.focusedRow() == OnlineRow::CreateRoom, -1.0f, true,
                      ui::kFontButton, ui::Tone::Lead)) {
      (void)menu.activate(at(OnlineRow::CreateRoom));
    }
    if (ui::pointerTakesFocus()) menu.setFocus(at(OnlineRow::CreateRoom));
    ui::bodyText("You host: pick the arena and the mode, then send friends the room's link.",
                 ui::kTextDim, ui::kFontSmall);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // Join a friend's room: the code field and Join on one line.
    sectionLabel("Join a friend's room");
    if (!codeEditing) {
      const std::string& code = menu.code();
      const std::size_t n = std::min(code.size(), codeBuf.size() - 1);
      std::memcpy(codeBuf.data(), code.data(), n);
      codeBuf[n] = '\0';
    }
    const ImVec2 fieldMin = ImGui::GetCursorScreenPos();
    ImGui::PushFont(ui::font(ui::Face::Display), ui::kFontButton);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(16.0f, (bigFrame - ui::kFontButton) * 0.5f));
    ImGui::SetNextItemWidth(std::min(190.0f, ImGui::GetContentRegionAvail().x * 0.45f));
    if (fields.editCode) {
      ImGui::SetKeyboardFocusHere();
      fields.editCode = false;
    }
    const auto filter = [](ImGuiInputTextCallbackData* data) -> int {
      return data->EventChar >= 0x80 || !isRoomCodeChar(static_cast<char>(data->EventChar)) ? 1 : 0;
    };
    ui::pushFieldStyle();
    const bool enter = ImGui::InputTextWithHint("##room_code", "CODE", codeBuf.data(), 6,
                                                ImGuiInputTextFlags_CharsUppercase |
                                                    ImGuiInputTextFlags_CallbackCharFilter |
                                                    ImGuiInputTextFlags_EnterReturnsTrue,
                                                filter);
    ui::popFieldStyle();
    ImGui::PopStyleVar();
    const ImVec2 fieldMax = ImGui::GetItemRectMax();
    ImGui::PopFont();
    const bool overCode = ui::pointerOverItem();
    if (ui::pointerTakesFocus(overCode) || ImGui::IsItemActivated()) {
      menu.setFocus(at(OnlineRow::Code));
    }
    codeEditing = ImGui::IsItemActive();
    if (ImGui::IsItemEdited()) menu.setCode(codeBuf.data());
    if (ui::showsFocus(menu.focusedRow() == OnlineRow::Code, overCode) || codeEditing) {
      ui::focusRing(fieldMin, fieldMax, ui::kRowRounding);
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
    if (ui::pointerTakesFocus()) menu.setFocus(at(OnlineRow::Join));
    ui::bodyText(menu.codeValid() ? "Ready: press Join."
                                  : "Type the 5-letter code from your friend's invite.",
                 ui::kTextDim, ui::kFontSmall);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ui::bigButton("Back", menu.focusedRow() == OnlineRow::Back, -1.0f)) out.back = true;
    if (ui::pointerTakesFocus()) menu.setFocus(at(OnlineRow::Back));
    const std::string where =
        "Up/Down: choose. Left/Right: region (\"no server\": every server there is busy right "
        "now). Enter: type the name or the code, or press the button. Esc / B: back.  Lobby: " +
        fields.lobby;
    footer(where.c_str());
  }
  ui::endScreen();
  return out;
}

void drawHowToPlay(const HowToPlay& card, bool backFocused, bool& back) {
  const float full = ImGui::GetContentRegionAvail().x;
  const bool wide = full >= 560.0f;
  ImFont* strong = ui::font(ui::Face::Strong);
  constexpr float kChip = ui::kFontSmall;
  // A binding on a pale-foam chip at the cursor, as wide as its text (at
  // most the cell), as an item.
  const auto chip = [&](const std::string& text) {
    const float cell = ImGui::GetContentRegionAvail().x;
    const float w = std::min(cell, ui::textWidth(kChip, text.c_str(), ui::Face::Strong) + 16.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + kChip + 8.0f), ui::kFoamPale, 10.0f);
    dl->PushClipRect(at, ImVec2(at.x + w - 4.0f, at.y + kChip + 8.0f), true);
    dl->AddText(strong, kChip, ImVec2(at.x + 8.0f, at.y + 4.0f), ui::kTealDeep, text.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(w, kChip + 8.0f));
  };
  // One row per control: the action in Lilita One with what it does under
  // it, then its keys and its pad buttons on chips (stacked when narrow).
  const int columns = wide ? 3 : 2;
  if (ImGui::BeginTable("how_to_play", columns, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch, wide ? 1.25f : 1.1f);
    ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    if (wide) ImGui::TableSetupColumn("pad", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TableNextColumn();
    ImGui::PushFont(strong, ui::kFontSmall);
    ImGui::TextColored(ui::vec4(ui::kTextDim), "%s", wide ? "Keyboard + mouse" : "Keys, then pad");
    if (wide) {
      ImGui::TableNextColumn();
      ImGui::TextColored(ui::vec4(ui::kTextDim), "Gamepad");
    }
    ImGui::PopFont();
    for (const ControlRow& r : card.rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushFont(ui::font(ui::Face::Display), ui::kFontUi + 3.0f);
      ImGui::TextColored(ui::vec4(ui::kTeal), "%s", r.action.c_str());
      ImGui::PopFont();
      ImGui::PushFont(nullptr, ui::kFontSmall);
      ImGui::TextWrapped("%s", r.what.c_str());
      ImGui::PopFont();
      ImGui::TableNextColumn();
      ImGui::Dummy(ImVec2(0.0f, 2.0f));
      chip(r.keyboard);
      if (wide) ImGui::TableNextColumn();
      if (wide) ImGui::Dummy(ImVec2(0.0f, 2.0f));
      chip(r.gamepad);
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  for (const std::string& tip : card.tips) {
    ui::bodyText(tip.c_str(), ui::kText, ui::kFontSmall);
  }
  ui::bodyText(card.secondKeyboard.c_str(), ui::kTextDim, ui::kFontSmall);
  ImGui::Spacing();
  if (ui::bigButton("Back", backFocused, -1.0f)) back = true;
  (void)ui::pointerTakesFocus();
}

bool drawHowToPlayScreen(const HowToPlay& card) {
  bool back = false;
  dimBackground();
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##how_to_play", std::min(780.0f, view.x - 24.0f))) {
    ui::heading("How to play");
    drawHowToPlay(card, true, back);
    footer("Esc / B / Enter: back");
  }
  ui::endScreen();
  return back;
}

void drawReplayLoading(const std::string& source) {
  dimBackground();
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##replay_loading", std::min(560.0f, view.x - 32.0f))) {
    // A small spinner so a slow download does not look frozen.
    constexpr const char* kSpin = "|/-\\";
    const auto spin = static_cast<std::size_t>(ImGui::GetTime() * 8.0) % 4;
    const std::string title = std::string("Loading replay\u2026  ") + kSpin[spin];
    ui::heading(title.c_str());
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    ImGui::TextWrapped("%s", source.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    footer("Esc: main menu");
  }
  ui::endScreen();
}

bool drawReplayError(const std::string& error, const std::string& source, ButtonMenu& button) {
  bool menu = false;
  button.setCount(1);
  dimBackground();
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  if (ui::beginScreen("##replay_error", std::min(640.0f, view.x - 32.0f))) {
    ui::heading("Could not open the replay", ui::kAlarm);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kText));
    ImGui::TextWrapped("%s", error.c_str());
    ImGui::PopStyleColor();
    ImGui::PushFont(nullptr, ui::kFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::vec4(ui::kTextDim));
    ImGui::TextWrapped("%s", source.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
    if (ui::bigButton("Main menu", button.focus() == 0, -1.0f, true, ui::kFontButton,
                      ui::Tone::Lead)) {
      menu = true;
    }
    (void)ui::pointerTakesFocus();
    footer("Enter / Esc: main menu");
  }
  ui::endScreen();
  return menu;
}

}  // namespace kfm::client
