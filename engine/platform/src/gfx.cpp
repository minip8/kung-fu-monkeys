#include "kfm/platform/gfx.hpp"

#include "kfm/platform/window.hpp"

#ifndef __EMSCRIPTEN__
#include <glad/gl.h>
#endif

#include <sokol_gfx.h>
#include <sokol_log.h>

#include <cstddef>

namespace kfm::platform {

namespace {

// The window's default framebuffer: RGBA8 colour, 24/8 depth-stencil, no
// MSAA (Window::init requests exactly that).
constexpr sg_pixel_format kColorFormat = SG_PIXELFORMAT_RGBA8;
constexpr sg_pixel_format kDepthFormat = SG_PIXELFORMAT_DEPTH_STENCIL;

}  // namespace

bool gfxInit() {
  sg_desc desc{};
  desc.environment.defaults.color_format = kColorFormat;
  desc.environment.defaults.depth_format = kDepthFormat;
  desc.environment.defaults.sample_count = 1;
  desc.logger.func = slog_func;
  sg_setup(&desc);
  return sg_isvalid();
}

void gfxShutdown() {
  if (sg_isvalid()) sg_shutdown();
}

void beginDefaultPass(const ClearColor& clear, int width, int height) {
  sg_pass pass{};
  pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
  pass.action.colors[0].clear_value = {.r = clear.r, .g = clear.g, .b = clear.b, .a = clear.a};
  pass.action.depth.load_action = SG_LOADACTION_CLEAR;
  pass.action.depth.clear_value = 1.0F;
  pass.action.stencil.load_action = SG_LOADACTION_CLEAR;
  pass.swapchain.width = width;
  pass.swapchain.height = height;
  pass.swapchain.sample_count = 1;
  pass.swapchain.color_format = kColorFormat;
  pass.swapchain.depth_format = kDepthFormat;
  pass.swapchain.gl.framebuffer = 0;
  pass.label = "kfm-default-pass";
  sg_begin_pass(&pass);
}

void endPass() {
  sg_end_pass();
  sg_commit();
}

void endFrame(Window& window) {
  endPass();
  window.swap();
}

void commitWithoutPresent() {
  sg_commit();
}

bool readPixelsRgb(int width, int height, std::span<std::uint8_t> out) {
#ifdef __EMSCRIPTEN__
  (void)width;
  (void)height;
  (void)out;
  return false;
#else
  if (width <= 0 || height <= 0) return false;
  const auto rowBytes = static_cast<std::size_t>(width) * 3U;
  const auto rows = static_cast<std::size_t>(height);
  if (out.size() < rowBytes * rows) return false;

  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(GL_BACK);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, out.data());
  const bool ok = glGetError() == GL_NO_ERROR;
  sg_reset_state_cache();  // we touched GL behind sokol's back

  // GL's origin is bottom-left; flip so row 0 is the top of the image.
  for (std::size_t top = 0, bottom = rows - 1; top < bottom; ++top, --bottom) {
    std::uint8_t* a = out.data() + (top * rowBytes);
    std::uint8_t* b = out.data() + (bottom * rowBytes);
    for (std::size_t i = 0; i < rowBytes; ++i) {
      const std::uint8_t t = a[i];
      a[i] = b[i];
      b[i] = t;
    }
  }
  return ok;
#endif
}

}  // namespace kfm::platform
