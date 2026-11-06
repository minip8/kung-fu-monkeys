#include "kfm/platform/debug_ui.hpp"

#include "kfm/platform/window.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <sokol_gfx.h>
#include <sokol_log.h>
#include <util/sokol_imgui.h>

#include <algorithm>

namespace kfm::platform {

namespace {
bool gInitialised = false;
}  // namespace

bool debugUiInit(const Window& window) {
  if (gInitialised) return true;
  // simgui_setup creates the ImGui context; the SDL3 backend attaches to it.
  simgui_desc_t desc{};
  desc.logger.func = slog_func;
  desc.ini_filename = nullptr;           // no imgui.ini next to the binary
  desc.disable_set_mouse_cursor = true;  // the SDL3 backend owns the cursor
  simgui_setup(&desc);
  if (!ImGui_ImplSDL3_InitForOpenGL(window.sdlWindow(), window.glContext())) {
    simgui_shutdown();
    return false;
  }
  ImGui::StyleColorsDark();
  gInitialised = true;
  return true;
}

void debugUiShutdown() {
  if (!gInitialised) return;
  ImGui_ImplSDL3_Shutdown();
  simgui_shutdown();  // destroys the ImGui context
  gInitialised = false;
}

void debugUiNewFrame(int width, int height, double dtSeconds, float pixelDensity) {
  // Mouse, keyboard, gamepad and cursor state from SDL; display size and
  // delta time from sokol_imgui, which then calls ImGui::NewFrame().
  ImGui_ImplSDL3_NewFrame();
  simgui_frame_desc_t frame{};
  frame.width = width;
  frame.height = height;
  frame.delta_time = dtSeconds > 0.0 ? dtSeconds : 1.0 / 60.0;
  frame.dpi_scale = pixelDensity;
  simgui_new_frame(&frame);
}

void debugUiFlush() {
  simgui_flush();
}

void debugUiDraw() {
  simgui_draw();
}

bool debugUiMeasuring() {
  const ImGuiContext* g = ImGui::GetCurrentContext();
  if (g == nullptr) return false;
  // HiddenFramesCannotSkipItems is set to 1 by Begin() on the frame a window
  // is created without a full size, and counted down by the next Begin().
  return std::ranges::any_of(g->Windows, [](const ImGuiWindow* w) {
    return (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) == 0 && w->Active &&
           w->Hidden && w->HiddenFramesCannotSkipItems > 0;
  });
}

bool debugUiWantsKeyboard() {
  return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

bool debugUiWantsMouse() {
  return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

}  // namespace kfm::platform
