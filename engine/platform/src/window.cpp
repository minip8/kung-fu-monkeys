#include "kfm/platform/window.hpp"

// No SDL_main: apps keep their own main(); SDL_SetMainReady covers the
// platforms where SDL would otherwise expect its entry point.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifndef __EMSCRIPTEN__
#include <glad/gl.h>
#endif

#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace kfm::platform {

namespace {

#ifndef __EMSCRIPTEN__
#ifdef __APPLE__
constexpr int kGlMinor = 1;
#else
constexpr int kGlMinor = 3;
#endif
#endif

bool setGlAttributes() {
  bool ok = true;
#ifdef __EMSCRIPTEN__
  // WebGL2 is OpenGL ES 3.0.
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
  // Core profile, forward compatible. macOS stops at 4.1 (sokol's Apple path
  // uses only 4.1 features). Elsewhere sokol GLCORE is compiled against the
  // GL 4.3 glad header (third_party/glad/README.md) and calls 4.2/4.3 entry
  // points, so ask for 4.3; drivers hand out their newest compatible version.
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kGlMinor);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
  ok = ok && SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  ok = ok && SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
  return ok;
}

}  // namespace

Window::~Window() {
  shutdown();
}

bool Window::init(const WindowConfig& config) {
  SDL_SetMainReady();
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    SDL_Log("kfm: SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  sdlInitialised_ = true;

  if (!setGlAttributes()) {
    SDL_Log("kfm: SDL_GL_SetAttribute failed: %s", SDL_GetError());
    shutdown();
    return false;
  }

#ifdef __EMSCRIPTEN__
  // The canvas fills the page and follows browser resizes; keyboard events
  // are bound to the canvas (focused by apps/client/shell.html).
  SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
  constexpr SDL_WindowFlags kFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                     SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_FILL_DOCUMENT;
#else
  constexpr SDL_WindowFlags kFlags =
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
  window_ = SDL_CreateWindow(config.title, config.width, config.height, kFlags);
  if (window_ == nullptr) {
    SDL_Log("kfm: SDL_CreateWindow failed: %s", SDL_GetError());
    shutdown();
    return false;
  }

  gl_ = SDL_GL_CreateContext(window_);
  if (gl_ == nullptr) {
    SDL_Log("kfm: SDL_GL_CreateContext failed: %s", SDL_GetError());
    shutdown();
    return false;
  }
  if (!SDL_GL_MakeCurrent(window_, gl_)) {
    SDL_Log("kfm: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
    shutdown();
    return false;
  }

#ifndef __EMSCRIPTEN__
  // SDL_FunctionPointer and GLADapiproc are both `void (*)(void)`.
  const int glVersion = gladLoadGL(SDL_GL_GetProcAddress);
  if (glVersion == 0 || GLAD_VERSION_MAJOR(glVersion) < 4 ||
      (GLAD_VERSION_MAJOR(glVersion) == 4 && GLAD_VERSION_MINOR(glVersion) < kGlMinor)) {
    SDL_Log("kfm: OpenGL 4.%d core is required (gladLoadGL returned %d.%d)", kGlMinor,
            GLAD_VERSION_MAJOR(glVersion), GLAD_VERSION_MINOR(glVersion));
    shutdown();
    return false;
  }
  SDL_Log("kfm: OpenGL %d.%d (%s)", GLAD_VERSION_MAJOR(glVersion), GLAD_VERSION_MINOR(glVersion),
          reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
#endif

  if (!SDL_GL_SetSwapInterval(config.vsync ? 1 : 0)) {
    // Not fatal: some drivers (and Emscripten) refuse; the loop still runs.
    SDL_Log("kfm: SDL_GL_SetSwapInterval failed: %s", SDL_GetError());
#ifndef __EMSCRIPTEN__
    if (config.vsync && config.paceWithoutVsync) {
      const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
      pacer_.setRate(mode != nullptr && mode->refresh_rate > 0.0F
                         ? static_cast<double>(mode->refresh_rate)
                         : FramePacer::kFallbackHz);
      SDL_Log("kfm: no vsync; pacing frames at %.2f Hz",
              1e9 / static_cast<double>(pacer_.periodNs()));
    }
#endif
  }
  SDL_StartTextInput(window_);  // ImGui text fields; harmless otherwise
  return true;
}

void Window::shutdown() {
  pacer_.setRate(0.0);
  if (gl_ != nullptr) {
    SDL_GL_DestroyContext(gl_);
    gl_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdlInitialised_) {
    SDL_Quit();
    sdlInitialised_ = false;
  }
}

DrawableSize Window::drawableSize() const {
  DrawableSize size;
  if (window_ != nullptr && !SDL_GetWindowSizeInPixels(window_, &size.width, &size.height)) {
    size = {};
  }
  return size;
}

float Window::pixelDensity() const {
  if (window_ == nullptr) return 1.0F;
  const float density = SDL_GetWindowPixelDensity(window_);
  return density > 0.0F ? density : 1.0F;
}

void Window::swap() {
  if (window_ == nullptr) return;
  SDL_GL_SwapWindow(window_);
  if (pacer_.active()) {
    const std::uint64_t wait = pacer_.waitAfterPresent(SDL_GetTicksNS());
    if (wait > 0) SDL_DelayPrecise(wait);
  }
}

std::uint64_t Window::nowNs() {
  return SDL_GetTicksNS();
}

bool Window::pollEvents(EventCallback callback, void* user) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (ImGui::GetCurrentContext() != nullptr) ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
      case SDL_EVENT_QUIT:
        quit_ = true;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (window_ != nullptr && event.window.windowID == SDL_GetWindowID(window_)) quit_ = true;
        break;
      default:
        // Resizes need no handling here: drawableSize() is queried every frame.
        break;
    }
    if (callback != nullptr) callback(user, event);
  }
  return !quit_;
}

}  // namespace kfm::platform
