#include "ui_style.hpp"

#include "core/menu_highlight.hpp"
#include "kfm/presentation/palette.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace kfm::client::assets {
// The embedded fonts (generated from assets/fonts/ by cmake/EmbedFile.cmake).
extern const unsigned char kUiFont[];  // Lilita One
extern const std::size_t kUiFontSize;
extern const unsigned char kUiFontBody[];  // Nunito Bold
extern const std::size_t kUiFontBodySize;
extern const unsigned char kUiFontStrong[];  // Nunito ExtraBold
extern const std::size_t kUiFontStrongSize;
}  // namespace kfm::client::assets

namespace kfm::client::ui {

namespace {

ImFont* gDebugFont = nullptr;
std::array<ImFont*, 3> gFonts{};  // by Face
const MenuHighlight* gHighlight = nullptr;

[[nodiscard]] int channel(float v) {
  return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

[[nodiscard]] int alphaOf(ImU32 c) {
  return static_cast<int>((c >> IM_COL32_A_SHIFT) & 0xFFu);
}

[[nodiscard]] ImFont* addFont(const unsigned char* bytes, std::size_t size) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // static, embedded bytes
  // ImGui reads the bytes and never writes them; the API takes void*.
  void* data = const_cast<unsigned char*>(bytes);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), kFontUi, &cfg);
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

ImFont* font(Face face) {
  ImFont* f = gFonts[static_cast<std::size_t>(face)];
  return f != nullptr ? f : ImGui::GetFont();
}

void applyUiStyle() {
  ImGuiIO& io = ImGui::GetIO();
  // sokol_imgui loaded ImGui's own font first: the debug overlay keeps it.
  gDebugFont = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
  gFonts[static_cast<std::size_t>(Face::Display)] = addFont(assets::kUiFont, assets::kUiFontSize);
  gFonts[static_cast<std::size_t>(Face::Body)] =
      addFont(assets::kUiFontBody, assets::kUiFontBodySize);
  gFonts[static_cast<std::size_t>(Face::Strong)] =
      addFont(assets::kUiFontStrong, assets::kUiFontStrongSize);
  if (ImFont* body = gFonts[static_cast<std::size_t>(Face::Body)]) io.FontDefault = body;

  ImGuiStyle& s = ImGui::GetStyle();
  s.FontSizeBase = kFontUi;
  s.WindowRounding = 20.0f;
  s.ChildRounding = 14.0f;
  s.FrameRounding = 12.0f;
  s.PopupRounding = 14.0f;
  s.GrabRounding = 10.0f;
  s.TabRounding = 10.0f;
  s.ScrollbarRounding = 10.0f;
  s.WindowBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;  // no ink: soft fills
  s.WindowPadding = ImVec2(28.0f, 22.0f);
  s.FramePadding = ImVec2(12.0f, 6.0f);
  s.ItemSpacing = ImVec2(10.0f, 8.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.SeparatorTextBorderSize = 2.0f;
  s.ScrollbarSize = 12.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = vec4(kText);
  c[ImGuiCol_TextDisabled] = vec4(kTextDim);
  c[ImGuiCol_WindowBg] = vec4(kPanelBg);
  c[ImGuiCol_ChildBg] = vec4(kInset);
  c[ImGuiCol_PopupBg] = vec4(withAlpha(kWhite, 250));
  c[ImGuiCol_Border] = vec4(kLine);
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_FrameBg] = vec4(kGround);
  c[ImGuiCol_FrameBgHovered] = vec4(kSandLight);
  c[ImGuiCol_FrameBgActive] = vec4(kSand);
  c[ImGuiCol_TitleBg] = vec4(kTealDeep);
  c[ImGuiCol_TitleBgActive] = vec4(kTeal);
  c[ImGuiCol_TitleBgCollapsed] = vec4(withAlpha(kTealDeep, 180));
  c[ImGuiCol_MenuBarBg] = vec4(kGround);
  c[ImGuiCol_ScrollbarBg] = vec4(withAlpha(kGround, 160));
  c[ImGuiCol_ScrollbarGrab] = vec4(kSand);
  c[ImGuiCol_ScrollbarGrabHovered] = vec4(kSandShade);
  c[ImGuiCol_ScrollbarGrabActive] = vec4(kBananaLip);
  c[ImGuiCol_CheckMark] = vec4(kTeal);
  c[ImGuiCol_SliderGrab] = vec4(kTeal);
  c[ImGuiCol_SliderGrabActive] = vec4(kTealDeep);
  c[ImGuiCol_Button] = vec4(kFoamPale);
  c[ImGuiCol_ButtonHovered] = vec4(kFoam);
  c[ImGuiCol_ButtonActive] = vec4(kFoamLip);
  c[ImGuiCol_Header] = vec4(kSandLight);
  c[ImGuiCol_HeaderHovered] = vec4(withAlpha(kSandLight, 200));
  c[ImGuiCol_HeaderActive] = vec4(kSand);
  c[ImGuiCol_Separator] = vec4(kLine);
  c[ImGuiCol_SeparatorHovered] = vec4(kSandShade);
  c[ImGuiCol_SeparatorActive] = vec4(kBananaLip);
  c[ImGuiCol_ResizeGrip] = vec4(withAlpha(kSand, 150));
  c[ImGuiCol_ResizeGripHovered] = vec4(kSandShade);
  c[ImGuiCol_ResizeGripActive] = vec4(kBananaLip);
  c[ImGuiCol_Tab] = vec4(kGround);
  c[ImGuiCol_TabHovered] = vec4(kSandLight);
  c[ImGuiCol_TabSelected] = vec4(kSand);
  c[ImGuiCol_PlotLines] = vec4(kTeal);
  c[ImGuiCol_PlotHistogram] = vec4(kOcean);
  c[ImGuiCol_TableHeaderBg] = vec4(kGround);
  c[ImGuiCol_TableBorderStrong] = vec4(kLine);
  c[ImGuiCol_TableBorderLight] = vec4(withAlpha(kLine, 160));
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = vec4(withAlpha(kGround, 160));
  c[ImGuiCol_TextSelectedBg] = vec4(withAlpha(kBanana, 160));
  c[ImGuiCol_NavCursor] = vec4(kCocoa);
}

ImFont* debugFont() {
  return gDebugFont;
}

void shadowText(ImDrawList* dl, float size, ImVec2 pos, ImU32 color, const char* text, Face face) {
  ImFont* f = font(face);
  const int alpha = alphaOf(color);
  const ImU32 ink = withAlpha(kOutline, alpha);
  // A soft drop under everything, then a thin cocoa outline round the glyphs.
  const float drop = std::max(1.0f, std::round(size * 0.06f));
  const float t = std::max(1.0f, std::round(size * 0.04f));
  dl->AddText(f, size, ImVec2(pos.x, pos.y + t + drop), withAlpha(kOutline, (alpha * 90) / 255),
              text);
  constexpr std::array<std::array<float, 2>, 8> kDirs{{{-1.0f, -1.0f},
                                                       {0.0f, -1.0f},
                                                       {1.0f, -1.0f},
                                                       {-1.0f, 0.0f},
                                                       {1.0f, 0.0f},
                                                       {-1.0f, 1.0f},
                                                       {0.0f, 1.0f},
                                                       {1.0f, 1.0f}}};
  for (const auto& d : kDirs) {
    dl->AddText(f, size, ImVec2(pos.x + (d[0] * t), pos.y + (d[1] * t)), ink, text);
  }
  dl->AddText(f, size, pos, color, text);
}

void centredText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text,
                 Face face) {
  const ImVec2 extent = font(face)->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  shadowText(dl, size, ImVec2(anchor.x - (extent.x * 0.5f), anchor.y - (extent.y * 0.5f)), color,
             text, face);
}

void pillText(ImDrawList* dl, float size, ImVec2 anchor, ImU32 color, const char* text, Face face) {
  ImFont* f = font(face);
  const ImVec2 extent = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float padX = std::round(size * 0.6f);
  const float padY = std::round(size * 0.18f);
  const ImVec2 lo{std::round(anchor.x - (extent.x * 0.5f) - padX),
                  std::round(anchor.y - (extent.y * 0.5f) - padY)};
  const ImVec2 hi{std::round(anchor.x + (extent.x * 0.5f) + padX),
                  std::round(anchor.y + (extent.y * 0.5f) + padY)};
  const float r = (hi.y - lo.y) * 0.5f;
  softShadow(dl, lo, hi, r, 2.0f, 6.0f, 40);
  dl->AddRectFilled(lo, hi, withAlpha(kHudPill, (alphaOf(kHudPill) * alphaOf(color)) / 255), r);
  dl->AddText(f, size, ImVec2(lo.x + padX, lo.y + padY), color, text);
}

float textWidth(float size, const char* text, Face face) {
  return font(face)->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

void softShadow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float offsetY, float blur,
                int alpha) {
  // Layers from the widest and faintest in: each adds a little, so the
  // middle reaches about `alpha` and the edge fades out over `blur`.
  constexpr int kLayers = 7;
  const int step = std::max(1, alpha / kLayers);
  for (int i = kLayers; i >= 1; --i) {
    const float grow = (blur * 0.5f) * (static_cast<float>(i) / static_cast<float>(kLayers));
    const ImVec2 lo{min.x - grow, min.y - grow + offsetY};
    const ImVec2 hi{max.x + grow, max.y + grow + offsetY};
    dl->AddRectFilled(lo, hi, withAlpha(kShadow, step), rounding + grow);
  }
}

void card(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 fill, float rounding, float shadow,
          int alpha) {
  if (shadow > 0.0f && alpha >= 255) {
    softShadow(dl, min, max, rounding, kPanelShadowY * shadow, kPanelShadowBlur * shadow,
               static_cast<int>(static_cast<float>(kPanelShadowAlpha) * std::min(shadow, 1.0f)));
  }
  dl->AddRectFilled(min, max, withAlpha(fill, (alphaOf(fill) * alpha) / 255), rounding);
}

bool beginScreen(const char* id, float width, float centreY, ImU32 fill) {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  constexpr float kMargin = 12.0f;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, (view.y - kPanelDrop) * centreY), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(width, view.x - (2.0f * kMargin)), 0.0f),
                           ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f),
      ImVec2(FLT_MAX, std::max(120.0f, view.y - (2.0f * kMargin) - kPanelDrop)));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBackground;
  // The window draws neither background nor border: the card below is both.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kPanelRounding);
  // Narrow windows (a phone, a 700 px canvas) get less padding.
  if (view.x < 760.0f) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
  const bool open = ImGui::Begin(id, nullptr, kFlags);
  ImGui::PopStyleVar(view.x < 760.0f ? 3 : 2);
  // The card goes on the background list: under every window (so under
  // the window's own scrollbar too) and over the HUD drawn before it.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  card(ImGui::GetBackgroundDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y), fill,
       kPanelRounding);
  return open;
}

void endScreen() {
  ImGui::End();
}

void menuBackdrop() {
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImU32 top = withAlpha(kGround, 150);
  const ImU32 bottom = kWash;
  dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), view, top, top, bottom, bottom);
}

namespace {
constexpr ImGuiWindowFlags kOverlayFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
// The overlays seen this frame and the ones open in ImGui: an overlay open
// but not drawn this frame is closed by finishOverlays() (a modal ImGui
// keeps open blocks every window under it).
constexpr std::size_t kMaxOverlays = 4;
std::array<const char*, kMaxOverlays> gOverlayIds{};
std::array<bool, kMaxOverlays> gOverlayDrawn{};

void noteOverlay(const char* id) {
  for (std::size_t i = 0; i < kMaxOverlays; ++i) {
    if (gOverlayIds[i] == nullptr || std::strcmp(gOverlayIds[i], id) == 0) {
      gOverlayIds[i] = id;
      gOverlayDrawn[i] = true;
      return;
    }
  }
}
}  // namespace

bool beginOverlay(const char* id, float width) {
  noteOverlay(id);
  if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
  const ImVec2 view = ImGui::GetIO().DisplaySize;
  constexpr float kMargin = 12.0f;
  ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, (view.y - kPanelDrop) * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(width, view.x - (2.0f * kMargin)), 0.0f),
                           ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f),
      ImVec2(FLT_MAX, std::max(120.0f, view.y - (2.0f * kMargin) - kPanelDrop)));
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, vec4(kScrim));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kPanelRounding);
  const bool narrow = view.x < 760.0f;
  if (narrow) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
  const bool open = ImGui::BeginPopupModal(id, nullptr, kOverlayFlags);
  ImGui::PopStyleVar(narrow ? 3 : 2);
  ImGui::PopStyleColor();
  if (!open) return false;
  // The card on the window's own list, under its items (the background list
  // would put it under the screen behind); its shadow reaches past the window.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRectFullScreen();
  card(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), kPanelBg, kPanelRounding);
  dl->PopClipRect();
  return true;
}

void endOverlay() {
  ImGui::EndPopup();
}

void finishOverlays() {
  for (std::size_t i = 0; i < kMaxOverlays; ++i) {
    const char* id = gOverlayIds[i];
    if (id == nullptr) continue;
    if (!gOverlayDrawn[i] && ImGui::IsPopupOpen(id)) {
      // Submitted once more, empty and without the dim, to close it.
      ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, vec4(kTransparent));
      if (ImGui::BeginPopupModal(id, nullptr, kOverlayFlags)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopStyleColor();
    }
    gOverlayDrawn[i] = false;
  }
}

void heading(const char* text, ImU32 color, float size) {
  ImGui::PushFont(font(Face::Display), size);
  ImGui::PushStyleColor(ImGuiCol_Text, vec4(color));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void bodyText(const char* text, ImU32 color, float size) {
  ImGui::PushFont(nullptr, size);
  ImGui::PushStyleColor(ImGuiCol_Text, vec4(color));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

float bigButtonHeight(float size) {
  return std::round(size + (2.0f * std::round(size * 0.34f)));
}

float bigButtonWidth(const char* label, float size) {
  const float padX = std::round(size * 0.9f);
  return textWidth(size, label, Face::Display) + (2.0f * padX);
}

void setMenuHighlight(const MenuHighlight* highlight) {
  gHighlight = highlight;
}

bool pointerMode() {
  return gHighlight == nullptr || !gHighlight->keys();
}

namespace {
[[nodiscard]] bool touchAware(bool hovered) {
  const ImGuiIO& io = ImGui::GetIO();
  return MenuHighlight::pointerOver(hovered, io.MouseSource == ImGuiMouseSource_TouchScreen,
                                    ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

// The focus ring: a white gap, then the cocoa ring, round [lo, hi].
void ringAround(ImDrawList* dl, ImVec2 lo, ImVec2 hi, float rounding) {
  const float g = kFocusGap;
  const float r = kFocusRingW;
  dl->AddRectFilled(ImVec2(lo.x - g - r, lo.y - g - r), ImVec2(hi.x + g + r, hi.y + g + r),
                    kFocusRing, rounding + g + r);
  dl->AddRectFilled(ImVec2(lo.x - g, lo.y - g), ImVec2(hi.x + g, hi.y + g), kWhite, rounding + g);
}
}  // namespace

bool pointerOverItem() {
  return touchAware(ImGui::IsItemHovered());
}

bool pointerOverRect(ImVec2 min, ImVec2 max) {
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                       ImGui::IsMouseHoveringRect(min, max);
  return touchAware(hovered);
}

namespace {
int gPointerFocusFrame = -1;  // the last frame an item took the focus from the pointer
}  // namespace

bool pointerTakesFocus() {
  return pointerTakesFocus(pointerOverItem());
}

bool pointerTakesFocus(bool pointerOver) {
  const bool takes = pointerMode() && pointerOver;
  if (takes) gPointerFocusFrame = ImGui::GetFrameCount();
  return takes;
}

bool pointerOnFocus() {
  return ImGui::GetCurrentContext() != nullptr && gPointerFocusFrame == ImGui::GetFrameCount();
}

bool showsFocus(bool focused, bool pointerOver) {
  return gHighlight != nullptr ? gHighlight->shows(focused, pointerOver) : pointerOver;
}

bool bigButton(const char* label, bool focused, float width, bool enabled, float size, Tone tone) {
  ImFont* f = font(Face::Display);
  const ImVec2 extent = f->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
  float w = width;
  if (w == 0.0f) w = bigButtonWidth(label, size);
  if (w < 0.0f) w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const float h = bigButtonHeight(size);
  // The item is the face and the lip under it; a focused button's raised
  // face and its ring poke into the item spacing round it.
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##big", ImVec2(w, h + kButtonDrop));
  ImGui::PopID();
  const bool over = enabled && pointerOverItem();
  const bool hot = showsFocus(focused, over);  // the ring and the lift
  const bool hovered = over && pointerMode();  // the lighter face
  const bool held = enabled && ImGui::IsItemActive();
  const ToneColors colors = enabled ? toneColors(tone) : kToneOff;
  const float rounding = h * 0.5f;  // a pill

  float lift = 0.0f;  // > 0 raises the face, < 0 sinks it onto the lip
  if (!enabled) {
    lift = -kButtonDrop;
  } else if (held) {
    lift = -(kButtonDrop - 1.0f);
  } else if (hot) {
    lift = kFocusLift;
  }
  const ImVec2 lo{at.x, at.y - lift};
  const ImVec2 hi{at.x + w, at.y + h - lift};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hot && enabled && !held) {
    ringAround(dl, lo, ImVec2(hi.x, at.y + h + kButtonDrop), rounding);
  }
  if (enabled) {
    dl->AddRectFilled(ImVec2(lo.x, at.y + kButtonDrop), ImVec2(hi.x, at.y + h + kButtonDrop),
                      colors.lip, rounding);
  }
  ImU32 fill = colors.fill;
  if (hovered && !held) fill = colors.hover;
  dl->AddRectFilled(lo, hi, fill, rounding);
  // A soft shine along the top of the face (the board's vinyl toy).
  if (enabled) {
    dl->AddRectFilled(ImVec2(lo.x + (rounding * 0.8f), lo.y + 3.0f),
                      ImVec2(hi.x - (rounding * 0.8f), lo.y + std::max(5.0f, h * 0.14f)),
                      withAlpha(kWhite, 45), h * 0.07f);
  }
  dl->AddText(f, size,
              ImVec2(std::round(lo.x + ((w - extent.x) * 0.5f)),
                     std::round(lo.y + ((h - extent.y) * 0.5f))),
              colors.text, label);
  return clicked && enabled;
}

void focusBar(ImVec2 min, ImVec2 max) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ringAround(dl, min, max, kRowRounding);
  dl->AddRectFilled(min, max, kFocusFill, kRowRounding);
}

namespace {
constexpr int kFieldColors = 7;
}  // namespace

void pushFieldStyle() {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, vec4(kGround));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, vec4(kSandLight));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, vec4(kWhite));
  ImGui::PushStyleColor(ImGuiCol_Text, vec4(kCocoa));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, vec4(kCocoaSoft));  // the hint
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, vec4(withAlpha(kBanana, 170)));
  ImGui::PushStyleColor(ImGuiCol_Border, vec4(kSand));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kRowRounding);
}

void popFieldStyle() {
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(kFieldColors);
}

void focusRing(ImVec2 min, ImVec2 max, float rounding) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float g = kFocusGap + (kFocusRingW * 0.5f);
  dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), kFocusRing, rounding + g,
              0, kFocusRingW);
}

bool arrowButton(const char* id, ImGuiDir dir, bool enabled) {
  const float side = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
  const bool hovered = enabled && pointerMode() && pointerOverItem();
  const bool held = enabled && ImGui::IsItemActive();
  const ToneColors colors = enabled ? toneColors(Tone::Plain) : kToneOff;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kDrop = 3.0f;
  const float sink = held || !enabled ? kDrop : 0.0f;
  const ImVec2 lo{at.x, at.y + sink};
  const ImVec2 hi{at.x + side, at.y + side - kDrop + sink};
  const float r = (hi.y - lo.y) * 0.5f;
  if (enabled && !held) {
    dl->AddRectFilled(ImVec2(lo.x, lo.y + kDrop), ImVec2(hi.x, hi.y + kDrop), colors.lip, r);
  }
  dl->AddRectFilled(lo, hi, hovered ? colors.hover : colors.fill, r);
  const ImVec2 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
  const float k = (hi.y - lo.y) * 0.22f;
  const ImU32 mark = enabled ? colors.text : withAlpha(colors.text, 110);
  if (dir == ImGuiDir_Left) {
    dl->AddTriangleFilled(ImVec2(c.x - k, c.y), ImVec2(c.x + (k * 0.8f), c.y - k),
                          ImVec2(c.x + (k * 0.8f), c.y + k), mark);
  } else {
    dl->AddTriangleFilled(ImVec2(c.x + k, c.y), ImVec2(c.x - (k * 0.8f), c.y + k),
                          ImVec2(c.x - (k * 0.8f), c.y - k), mark);
  }
  return clicked && enabled;
}

}  // namespace kfm::client::ui
