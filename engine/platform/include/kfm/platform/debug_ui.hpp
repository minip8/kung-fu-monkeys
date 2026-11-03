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

// Whether ImGui wants the keyboard / mouse this frame: when true, the app
// should not feed those events to gameplay input.
[[nodiscard]] bool debugUiWantsKeyboard();
[[nodiscard]] bool debugUiWantsMouse();

}  // namespace kfm::platform
