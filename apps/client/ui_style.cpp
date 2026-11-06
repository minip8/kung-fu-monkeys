#include "ui_style.hpp"

#include "kfm/presentation/palette.hpp"

#include <algorithm>
#include <array>
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
  s.FrameBorderSize = 2.0f;  // ink round every field, as round every button
  s.WindowPadding = ImVec2(24.0f, 18.0f);
  s.FramePadding = ImVec2(12.0f, 6.0f);
  s.ItemSpacing = ImVec2(10.0f, 8.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.SeparatorTextBorderSize = 2.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = vec4(kCream);
  c[ImGuiCol_TextDisabled] = vec4(kCreamDim);
  c[ImGuiCol_WindowBg] = vec4(kPanelBg);
  c[ImGuiCol_ChildBg] = vec4(kBarkInset);
  c[ImGuiCol_PopupBg] = vec4(withAlpha(kBark, 250));
  c[ImGuiCol_Border] = vec4(kOutline);
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
  c[ImGuiCol_ButtonHovered] = vec4(kSandLight);
  c[ImGuiCol_ButtonActive] = vec4(kGold);
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
  const auto alpha = static_cast<int>((color >> IM_COL32_A_SHIFT) & 0xFFu);
  const ImU32 ink = withAlpha(kOutline, alpha);
  // The drop: under everything, a little lower than the outline reaches.
  const float drop = std::max(1.0f, std::round(size * 0.07f));
  {
    const float t = std::max(1.0f, std::round(size * 0.05f));
    dl->AddText(font, size, ImVec2(pos.x, pos.y + t + drop), ink, text);
    // The outline: eight copies round the glyphs.
    constexpr std::array<std::array<float, 2>, 8> kDirs{{{-1.0f, -1.0f},
                                                         {0.0f, -1.0f},
                                                         {1.0f, -1.0f},
                                                         {-1.0f, 0.0f},
                                                         {1.0f, 0.0f},
                                                         {-1.0f, 1.0f},
                                                         {0.0f, 1.0f},
                                                         {1.0f, 1.0f}}};
    for (const auto& d : kDirs) {
      dl->AddText(font, size, ImVec2(pos.x + (d[0] * t), pos.y + (d[1] * t)), ink, text);
    }
  }
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

void card(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, float rounding, float border,
          float drop, int alpha) {
  const auto scaled = [alpha](ImU32 c) {
    const auto a = static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFFu);
    return withAlpha(c, (a * alpha) / 255);
  };
  if (drop > 0.0f) {
    dl->AddRectFilled(ImVec2(min.x, min.y + drop), ImVec2(max.x, max.y + drop), scaled(kOutline),
                      rounding);
  }
  if (border <= 0.0f) {
    dl->AddRectFilled(min, max, scaled(fill), rounding);
    return;
  }
  // The border is an ink card with the face inset into it: a stroked
  // outline would leave the face's anti-aliased edge showing outside it.
  dl->AddRectFilled(min, max, scaled(kOutline), rounding);
  dl->AddRectFilled(ImVec2(min.x + border, min.y + border), ImVec2(max.x - border, max.y - border),
                    scaled(fill), std::max(0.0f, rounding - border));
}

bool beginScreen(const char* id, float width, float centreY) {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, (view.y - kPanelDrop) * centreY), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(width, view.x - 24.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(120.0f, view.y - 24.0f - kPanelDrop)));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBackground;
  // The window draws neither background nor border: the panel card below is both.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kPanelRounding);
  const bool open = ImGui::Begin(id, nullptr, kFlags);
  ImGui::PopStyleVar(2);
  // The card goes on the background list: under every window (so under
  // the window's own scrollbar too) and over the HUD drawn before it; its
  // slab hangs kPanelDrop under the window.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  card(ImGui::GetBackgroundDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y), kPanelBg,
       kPanelRounding, kPanelBorder, kPanelDrop);
  return open;
}

void endScreen() {
  ImGui::End();
}

void heading(const char* text, ImU32 color, float size) {
  ImGui::PushFont(nullptr, size);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  shadowText(ImGui::GetWindowDrawList(), size, pos, color, text);
  ImGui::Dummy(ImVec2(textWidth(size, text), ImGui::GetFontSize() + 4.0f));
  ImGui::PopFont();
}

float bigButtonHeight(float size) {
  return std::round(size + (2.0f * (std::round(size * 0.3f) + kButtonBorder)));
}

bool bigButton(const char* label, bool focused, float width, bool enabled, float size, Tone tone) {
  const float padX = std::round(size * 0.6f);
  const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
  float w = width;
  if (w == 0.0f) w = extent.x + (2.0f * (padX + kButtonBorder));
  if (w < 0.0f) w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const float h = bigButtonHeight(size);
  // The item is the face and the slab under it; a focused button's raised
  // face and its ring poke into the item spacing above.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##big", ImVec2(w, h + kButtonDrop));
  ImGui::PopID();
  const bool hovered = enabled && ImGui::IsItemHovered();
  const bool held = enabled && ImGui::IsItemActive();
  const ToneColors colors = enabled ? toneColors(tone) : kToneOff;

  float lift = 0.0f;  // > 0 raises the face, < 0 sinks it onto the slab
  if (!enabled) {
    lift = -kButtonDrop;
  } else if (held) {
    lift = -(kButtonDrop - 1.0f);
  } else if (focused) {
    lift = kFocusLift;
  }
  const ImVec2 lo{at.x, at.y - lift};
  const ImVec2 hi{at.x + w, at.y + h - lift};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (enabled) {
    dl->AddRectFilled(ImVec2(lo.x, at.y + kButtonDrop), ImVec2(hi.x, at.y + h + kButtonDrop),
                      kOutline, kButtonRounding);
  }
  if (focused && enabled && !held) {
    const float r = kFocusRingW;
    dl->AddRectFilled(ImVec2(lo.x - r, lo.y - r), ImVec2(hi.x + r, hi.y + r), kFocusRing,
                      kButtonRounding + r);
  }
  ImU32 fill = colors.fill;
  if (held) {
    fill = colors.pressed;
  } else if (hovered) {
    fill = colors.hover;
  }
  card(dl, lo, hi, fill, kButtonRounding, kButtonBorder, 0.0f);
  dl->AddText(ImGui::GetFont(), size,
              ImVec2(std::round(lo.x + ((w - extent.x) * 0.5f)),
                     std::round(lo.y + ((h - extent.y) * 0.5f))),
              colors.text, label);
  return clicked && enabled;
}

void focusBar(ImVec2 min, ImVec2 max) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kRounding = 12.0f;
  const float r = kFocusRingW;
  dl->AddRectFilled(ImVec2(min.x - r, min.y - r), ImVec2(max.x + r, max.y + r), kFocusRing,
                    kRounding + r);
  card(dl, min, max, kBarkLight, kRounding, 2.0f, 0.0f);
}

namespace {
constexpr int kFieldColors = 6;
}  // namespace

void pushFieldStyle() {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, vec4(kCream));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, vec4(kSandLight));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, vec4(kCream));
  ImGui::PushStyleColor(ImGuiCol_Text, vec4(kBark));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, vec4(withAlpha(kBark, 130)));  // the hint
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, vec4(withAlpha(kMango, 150)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
}

void popFieldStyle() {
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(kFieldColors);
}

void focusRing(ImVec2 min, ImVec2 max, float rounding) {
  const float r = kFocusRingW;
  ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - (r * 0.5f), min.y - (r * 0.5f)),
                                      ImVec2(max.x + (r * 0.5f), max.y + (r * 0.5f)), kFocusRing,
                                      rounding + (r * 0.5f), 0, r);
}

bool arrowButton(const char* id, ImGuiDir dir) {
  const float side = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  const ToneColors colors = toneColors(Tone::Plain);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kDrop = 2.0f;
  constexpr float kRounding = 7.0f;
  const float sink = held ? kDrop : 0.0f;
  const ImVec2 lo{at.x, at.y + sink};
  const ImVec2 hi{at.x + side, at.y + side - kDrop + sink};
  ImU32 fill = hovered ? colors.hover : colors.fill;
  if (held) fill = colors.pressed;
  card(dl, lo, hi, fill, kRounding, 2.0f, held ? 0.0f : kDrop);
  const ImVec2 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
  const float k = (hi.y - lo.y) * 0.22f;
  if (dir == ImGuiDir_Left) {
    dl->AddTriangleFilled(ImVec2(c.x - k, c.y), ImVec2(c.x + (k * 0.8f), c.y - k),
                          ImVec2(c.x + (k * 0.8f), c.y + k), colors.text);
  } else {
    dl->AddTriangleFilled(ImVec2(c.x + k, c.y), ImVec2(c.x - (k * 0.8f), c.y + k),
                          ImVec2(c.x - (k * 0.8f), c.y - k), colors.text);
  }
  return clicked;
}

}  // namespace kfm::client::ui
