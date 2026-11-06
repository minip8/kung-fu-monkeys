// sokol_gfx lifetime and the default (window) pass (architecture §7).
//
// The renderer in engine/presentation does not include this header: it links
// kfm::sokol and includes <sokol_gfx.h> itself; the backend define is a PUBLIC
// compile definition of kfm::sokol, so both sides agree. A frame is:
//
//   debugUiFlush();
//   beginDefaultPass(clear, w, h);  ... renderer draws ... debugUiDraw();
//   endFrame(window);                // sg_end_pass + sg_commit + swap
//
// or, to read the frame back before presenting:
//   endPass(); readPixelsRgb(w, h, buf); window.swap();
#pragma once

#include <cstdint>
#include <span>

namespace kfm::platform {

class Window;

struct ClearColor {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 1.0F;
};

// sg_setup for the window's GL context (call after Window::init). Returns
// false if sokol_gfx failed to initialise.
[[nodiscard]] bool gfxInit();
void gfxShutdown();

// Begins the swapchain pass over the default framebuffer (w × h pixels,
// Window::drawableSize()), clearing colour, depth and stencil.
void beginDefaultPass(const ClearColor& clear, int width, int height);
// sg_end_pass + sg_commit.
void endPass();
// endPass() then Window::swap().
void endFrame(Window& window);
// Ends a frame that draws nothing and presents nothing (sg_commit only): the
// window keeps showing the previous frame. For a frame that is not ready to
// be seen (debugUiMeasuring()).
void commitWithoutPresent();

// Reads the back buffer of the default framebuffer as tightly packed RGB8,
// top row first. Call after endPass() and before swap. `out` must hold
// width * height * 3 bytes. Native only: returns false under Emscripten.
[[nodiscard]] bool readPixelsRgb(int width, int height, std::span<std::uint8_t> out);

}  // namespace kfm::platform
