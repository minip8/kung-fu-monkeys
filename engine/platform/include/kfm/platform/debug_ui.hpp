// Dear ImGui debug overlay glue (architecture §7): the SDL3 platform backend
// for input and sokol_imgui for drawing. Apps include <imgui.h> directly and
// build their windows between debugUiNewFrame() and debugUiFlush().
//
//   debugUiInit(window);                      // after gfxInit()
//   per frame:
//     debugUiNewFrame(w, h, dt, density);
//     ImGui::...;
//     debugUiFlush();                         // outside any sokol pass
//     beginDefaultPass(...); ...; debugUiDraw(); endFrame(window);
//   debugUiShutdown();                        // before gfxShutdown()
#pragma once

namespace kfm::platform {

class Window;

[[nodiscard]] bool debugUiInit(const Window& window);
void debugUiShutdown();

// `width`/`height` are framebuffer pixels, `pixelDensity` is
// Window::pixelDensity(), `dtSeconds` the frame time.
void debugUiNewFrame(int width, int height, double dtSeconds, float pixelDensity);
// Ends the ImGui frame and uploads vertices/textures. Must be called outside
// a sokol pass (sokol_imgui's rule), after the frame's ImGui calls.
void debugUiFlush();
// Draws the flushed UI into the current sokol pass (call last in the pass).
void debugUiDraw();
// After debugUiFlush(): whether a window created this frame is still
// measuring itself. ImGui hides a new window whose size is not fully given
// (an auto-height screen) for its first frame while it measures its
// contents, so that frame shows the scene without the window; an app may
// hold the frame back (commitWithoutPresent()) instead of presenting it.
// Popups and tooltips do not count: their hidden frame shows nothing amiss.
[[nodiscard]] bool debugUiMeasuring();

// Whether ImGui wants the keyboard / mouse this frame: when true, the app
// should not feed those events to gameplay input.
[[nodiscard]] bool debugUiWantsKeyboard();
[[nodiscard]] bool debugUiWantsMouse();

}  // namespace kfm::platform
