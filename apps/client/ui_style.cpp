#include "ui_style.hpp"

#include "kfm/presentation/palette.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>

namespace kfm::client::assets {
// The embedded font (ui_font.cpp, generated from assets/fonts/).
extern const unsigned char kUiFont[];
extern const std::size_t kUiFontSize;
}  // namespace kfm::client::assets

namespace kfm::client::ui {

namespace {

ImFont* gDebugFont = nullptr;

[[nodiscard]] int channel(float v) {
  return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

ImVec4 vec4(ImU32 c) {
  return ImGui::ColorConvertU32ToFloat4(c);
}

ImU32 withAlpha(ImU32 c, int alpha) {
  return (c & ~IM_COL32_A_MASK) |
         (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << IM_COL32_A_SHIFT);
}

ImU32 slotColor(uint32_t slot, int alpha) {
  const presentation::Color c = presentation::playerColor(slot);
  return IM_COL32(channel(c.r), channel(c.g), channel(c.b), alpha);
}

ImVec4 slotColorVec(uint32_t slot) {
  const presentation::Color c = presentation::playerColor(slot);
  return {c.r, c.g, c.b, 1.0f};
}

void applyUiStyle() {
  ImGuiIO& io = ImGui::GetIO();
  // sokol_imgui loaded ImGui's own font first: the debug overlay keeps it.
  gDebugFont = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // static, embedded bytes
  // ImGui reads the bytes and never writes them; the API takes void*.
  void* data =
      const_cast<unsigned char*>(assets::kUiFont);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  ImFont* font =
      io.Fonts->AddFontFromMemoryTTF(data, static_cast<int>(assets::kUiFontSize), kFontUi, &cfg);
  if (font != nullptr) io.FontDefault = font;

  ImGuiStyle& s = ImGui::GetStyle();
  s.FontSizeBase = kFontUi;
  s.WindowRounding = 16.0f;
  s.ChildRounding = 10.0f;
  s.FrameRounding = 10.0f;
  s.PopupRounding = 10.0f;
  s.GrabRounding = 8.0f;
  s.TabRounding = 8.0f;
  s.ScrollbarRounding = 8.0f;
  s.WindowBorderSize = 3.0f;
  s.FrameBorderSize = 0.0f;
  s.WindowPadding = ImVec2(22.0f, 18.0f);
  s.FramePadding = ImVec2(12.0f, 6.0f);
  s.ItemSpacing = ImVec2(10.0f, 8.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.SeparatorTextBorderSize = 2.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = vec4(kCream);
  c[ImGuiCol_TextDisabled] = vec4(kCreamDim);
  c[ImGuiCol_WindowBg] = vec4(withAlpha(kBark, 245));
  c[ImGuiCol_ChildBg] = vec4(withAlpha(kBarkLight, 110));
  c[ImGuiCol_PopupBg] = vec4(withAlpha(kBark, 250));
  c[ImGuiCol_Border] = vec4(kInk);
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_FrameBg] = vec4(kBarkLight);
  c[ImGuiCol_FrameBgHovered] = vec4(withAlpha(kMango, 170));
  c[ImGuiCol_FrameBgActive] = vec4(kMangoDeep);
  c[ImGuiCol_TitleBg] = vec4(kInk);
  c[ImGuiCol_TitleBgActive] = vec4(kInk);
  c[ImGuiCol_TitleBgCollapsed] = vec4(withAlpha(kInk, 180));
  c[ImGuiCol_MenuBarBg] = vec4(kBarkLight);
  c[ImGuiCol_ScrollbarBg] = vec4(withAlpha(kBark, 120));
  c[ImGuiCol_ScrollbarGrab] = vec4(kBarkLight);
  c[ImGuiCol_ScrollbarGrabHovered] = vec4(kMango);
  c[ImGuiCol_ScrollbarGrabActive] = vec4(kGold);
  c[ImGuiCol_CheckMark] = vec4(kGold);
  c[ImGuiCol_SliderGrab] = vec4(kGold);
  c[ImGuiCol_SliderGrabActive] = vec4(kMango);
  c[ImGuiCol_Button] = vec4(kSand);
  c[ImGuiCol_ButtonHovered] = vec4(kMangoLight);
  c[ImGuiCol_ButtonActive] = vec4(kMangoDeep);
  c[ImGuiCol_Header] = vec4(withAlpha(kMango, 150));
  c[ImGuiCol_HeaderHovered] = vec4(withAlpha(kMango, 110));
  c[ImGuiCol_HeaderActive] = vec4(kMangoDeep);
  c[ImGuiCol_Separator] = vec4(withAlpha(kInk, 220));
  c[ImGuiCol_SeparatorHovered] = vec4(kMango);
  c[ImGuiCol_SeparatorActive] = vec4(kGold);
  c[ImGuiCol_ResizeGrip] = vec4(withAlpha(kMango, 120));
  c[ImGuiCol_ResizeGripHovered] = vec4(kMangoLight);
  c[ImGuiCol_ResizeGripActive] = vec4(kGold);
  c[ImGuiCol_Tab] = vec4(kBarkLight);
  c[ImGuiCol_TabHovered] = vec4(kMangoLight);
  c[ImGuiCol_TabSelected] = vec4(kMango);
  c[ImGuiCol_PlotLines] = vec4(kGold);
  c[ImGuiCol_PlotHistogram] = vec4(kGold);
  c[ImGuiCol_TableHeaderBg] = vec4(kBarkLight);
  c[ImGuiCol_TableBorderStrong] = vec4(withAlpha(kInk, 220));
  c[ImGuiCol_TableBorderLight] = vec4(withAlpha(kInk, 140));
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = vec4(withAlpha(kBarkLight, 80));
  c[ImGuiCol_TextSelectedBg] = vec4(withAlpha(kMango, 120));
  c[ImGuiCol_NavCursor] = vec4(kGold);
}

ImFont* debugFont() {
  return gDebugFont;
}

void shadowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text) {
  ImFont* font = ImGui::GetFont();
  const float off = std::max(1.0f, size * 0.06f);
  dl->AddText(font, size, ImVec2(pos.x + off, pos.y + off), kShadow, text);
  dl->AddText(font, size, pos, color, text);
}

void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text) {
  const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  shadowText(dl, size, ImVec2(anchor.x - (extent.x * 0.5f), anchor.y - (extent.y * 0.5f)), color,
             text);
}

float textWidth(float size, const char* text) {
  return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

bool beginScreen(const char* id, float width, float bgAlpha) {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(width, view.x - 24.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                      ImVec2(FLT_MAX, std::max(120.0f, view.y - 24.0f)));
  ImGui::SetNextWindowBgAlpha(bgAlpha);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings;
  return ImGui::Begin(id, nullptr, kFlags);
}

void endScreen() {
  ImGui::End();
}

void heading(const char* text, ImU32 color, float size) {
  ImGui::PushFont(nullptr, size);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  shadowText(ImGui::GetWindowDrawList(), size, pos, color, text);
  ImGui::Dummy(ImVec2(textWidth(size, text), ImGui::GetFontSize()));
  ImGui::PopFont();
}

bool bigButton(const char* label, bool focused, float width, bool enabled, float size) {
  ImGui::PushFont(nullptr, size);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 14.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
  constexpr int kVars = 3;
  constexpr int kColors = 5;
  ImGui::PushStyleColor(ImGuiCol_Border, vec4(kInk));
  if (focused) {
    ImGui::PushStyleColor(ImGuiCol_Button, vec4(kMango));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec4(kMangoLight));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec4(kMangoDeep));
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(kInk));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, vec4(kSand));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, vec4(kMangoLight));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, vec4(kMangoDeep));
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(kBark));
  }
  // The chunky drop shadow: an ink slab under the button, kDrop lower. The
  // button sets its own size, so the slab is drawn after it into a channel
  // behind it; ItemSpacing (8 px) leaves room for the drop below.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  if (!enabled) ImGui::BeginDisabled();
  const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
  if (!enabled) ImGui::EndDisabled();
  constexpr float kDrop = 5.0f;
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  dl->ChannelsSetCurrent(0);
  dl->AddRectFilled(ImVec2(lo.x, lo.y + kDrop), ImVec2(hi.x, hi.y + kDrop),
                    withAlpha(kInk, enabled ? 255 : 120), 14.0f);
  dl->ChannelsMerge();
  ImGui::PopStyleColor(kColors);
  ImGui::PopStyleVar(kVars);
  ImGui::PopFont();
  return clicked && enabled;
}

void focusBar(ImVec2 min, ImVec2 max) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(min, max, withAlpha(kMango, 60), 8.0f);
  dl->AddRect(min, max, withAlpha(kMango, 220), 8.0f, 0, 2.0f);
}

}  // namespace kfm::client::ui
