// SDL3 window + GL context + event pump (architecture §7, §13). The only
// header of kfm_platform that exposes SDL: the per-event callback receives the
// raw SDL_Event so the app can hand it to the input sampler.
//
// Input seam: pass an EventCallback to pollEvents(). It sees every event
// (after Dear ImGui did), in arrival order, with SDL_Event::common.timestamp
// untouched. That timestamp is on the same clock as Window::nowNs()
// (SDL_GetTicksNS), which is what the input sampler needs to derive sub-tick
// stamps (§5.11). The translation SDL_Event -> kfm::platform::InputEvent lives
// in the app (or a later platform helper), not here.
#pragma once

#include "kfm/platform/frame_pacer.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include <cstdint>

namespace kfm::platform {

struct WindowConfig {
  const char* title = "Kung Fu Monkeys";
  int width = 1280;  // logical (window) size, not pixels
  int height = 720;
  bool vsync = true;  // swap interval 1; Emscripten always follows requestAnimationFrame
  // Natively, when vsync was asked for but the swap interval cannot be set,
  // swap() waits out the rest of the display's refresh period (FramePacer) so
  // the loop runs at the display rate instead of as fast as it can.
  bool paceWithoutVsync = false;
};

struct DrawableSize {
  int width = 0;  // framebuffer pixels (differs from the window size on high-DPI displays)
  int height = 0;
};

// Called once per SDL_Event from pollEvents(). `user` is the pointer given to pollEvents.
using EventCallback = void (*)(void* user, const SDL_Event& event);

class Window {
 public:
  Window() = default;
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;
  Window(Window&&) = delete;
  Window& operator=(Window&&) = delete;
  ~Window();

  // Initialises SDL video + gamepad, creates a resizable, high-DPI window and a
  // GL context (core forward-compatible: 4.3, 4.1 on macOS; ES 3.0 = WebGL2 under
  // Emscripten) and, natively, loads GL through glad. Returns false and logs
  // the SDL error on failure.
  [[nodiscard]] bool init(const WindowConfig& config);
  void shutdown();  // idempotent; also run by the destructor

  [[nodiscard]] DrawableSize drawableSize() const;
  // Framebuffer pixels per logical window unit (1 on normal displays).
  [[nodiscard]] float pixelDensity() const;
  // Presents the frame; then, when pacing (WindowConfig::paceWithoutVsync and
  // no vsync), waits until the next frame is due.
  void swap();

  // Monotonic nanoseconds (SDL_GetTicksNS): the clock of SDL event timestamps.
  [[nodiscard]] static std::uint64_t nowNs();

  // Drains all pending events. Each event is first given to Dear ImGui (when a
  // context exists), then to `callback` (may be null). SDL_EVENT_QUIT and
  // closing the window set quitRequested(). Returns !quitRequested().
  bool pollEvents(EventCallback callback, void* user);

  [[nodiscard]] bool quitRequested() const { return quit_; }
  void requestQuit() { quit_ = true; }

  [[nodiscard]] SDL_Window* sdlWindow() const { return window_; }
  [[nodiscard]] SDL_GLContext glContext() const { return gl_; }

 private:
  SDL_Window* window_ = nullptr;
  SDL_GLContext gl_ = nullptr;
  bool sdlInitialised_ = false;
  bool quit_ = false;
  FramePacer pacer_;
};

}  // namespace kfm::platform
