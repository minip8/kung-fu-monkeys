// kfm_client: the playable client (architecture §3, §11 M2). This first cut
// opens the window, clears it, and draws a Dear ImGui overlay; the sim,
// renderer and input sampler are wired in by later M2 work units.
//
//   kfm_client [--exit-after-frames N] [--screenshot out.ppm]
//
// --exit-after-frames N renders N frames and exits 0 (rendering smoke test).
// --screenshot path     writes the last rendered frame as binary PPM (native only;
//                       with no --exit-after-frames it is taken when the client quits).
#include "kfm/platform/debug_ui.hpp"
#include "kfm/platform/gfx.hpp"
#include "kfm/platform/main_loop.hpp"
#include "kfm/platform/window.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <imgui.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace kfm;

struct Options {
  std::uint64_t exitAfterFrames = 0;  // 0: run until quit
  const char* screenshotPath = nullptr;
};

struct App {
  Options options;
  platform::Window window;
  std::uint64_t frames = 0;
  std::uint64_t lastFrameNs = 0;
  bool quit = false;
  int exitCode = EXIT_SUCCESS;
};

// Static storage: under Emscripten main() is unwound once the loop starts.
App gApp;

constexpr const char* kUsage = "usage: kfm_client [--exit-after-frames N] [--screenshot out.ppm]\n";

bool parseArgs(int argc, const char* const* argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--exit-after-frames" && i + 1 < argc) {
      const std::string_view v = argv[++i];
      const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), o.exitAfterFrames);
      if (ec != std::errc{} || end != v.data() + v.size() || o.exitAfterFrames == 0) {
        std::fprintf(stderr, "error: --exit-after-frames needs a positive number, got '%s'\n",
                     argv[i]);
        return false;
      }
    } else if (arg == "--screenshot" && i + 1 < argc) {
      o.screenshotPath = argv[++i];
    } else {
      std::fprintf(stderr, "error: unknown or incomplete argument '%s'\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Event seam: the input sampler's SDL_Event -> InputEvent translation hooks in
// here in the next M2 wave (respecting debugUiWantsKeyboard/Mouse).
void onEvent(void* user, const SDL_Event& event) {
  auto* app = static_cast<App*>(user);
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) app->quit = true;
}

bool writePpm(const char* path, int width, int height) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 3U);
  if (!platform::readPixelsRgb(width, height, pixels)) {
    std::fprintf(stderr, "error: reading the framebuffer failed\n");
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "error: cannot open '%s' for writing\n", path);
    return false;
  }
  out << "P6\n" << width << ' ' << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
  return static_cast<bool>(out.flush());
}

bool frame(void* user) {
  auto* app = static_cast<App*>(user);
  if (!app->window.pollEvents(onEvent, app)) app->quit = true;

  const std::uint64_t now = platform::Window::nowNs();
  const double dt = static_cast<double>(now - app->lastFrameNs) * 1e-9;
  app->lastFrameNs = now;

  const platform::DrawableSize size = app->window.drawableSize();
  platform::debugUiNewFrame(size.width, size.height, dt, app->window.pixelDensity());

  ImGui::SetNextWindowPos(ImVec2(16.0F, 16.0F), ImGuiCond_FirstUseEver);
  ImGui::Begin("Kung Fu Monkeys");
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::Text("frame time %.2f ms",
              static_cast<double>(io.Framerate > 0.0F ? 1000.0F / io.Framerate : 0.0F));
  ImGui::Text("fps %.1f", static_cast<double>(io.Framerate));
  ImGui::Text("drawable %d x %d", size.width, size.height);
  if (ImGui::Button("Quit")) app->quit = true;
  ImGui::End();
  platform::debugUiFlush();

  platform::beginDefaultPass({.r = 0.18F, .g = 0.42F, .b = 0.27F, .a = 1.0F}, size.width,
                             size.height);
  platform::debugUiDraw();
  platform::endPass();

  ++app->frames;
  const bool lastFrame = app->quit || (app->options.exitAfterFrames != 0 &&
                                       app->frames >= app->options.exitAfterFrames);
  if (lastFrame && app->options.screenshotPath != nullptr &&
      !writePpm(app->options.screenshotPath, size.width, size.height)) {
    app->exitCode = EXIT_FAILURE;
  }
  app->window.swap();
  return !lastFrame;
}

void onExit(void* user) {
  auto* app = static_cast<App*>(user);
  platform::debugUiShutdown();
  platform::gfxShutdown();
  app->window.shutdown();
#ifdef __EMSCRIPTEN__
  std::printf("kfm_client: stopped after %llu frames\n",
              static_cast<unsigned long long>(app->frames));
#endif
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  App& app = gApp;
  if (!parseArgs(argc, argv, app.options)) {
    std::fputs(kUsage, stderr);
    return EXIT_FAILURE;
  }
  if (!app.window.init({.title = "Kung Fu Monkeys", .width = 1280, .height = 720, .vsync = true})) {
    return EXIT_FAILURE;
  }
  if (!platform::gfxInit()) {
    std::fprintf(stderr, "error: sokol_gfx failed to initialise\n");
    return EXIT_FAILURE;
  }
  if (!platform::debugUiInit(app.window)) {
    std::fprintf(stderr, "error: Dear ImGui failed to initialise\n");
    platform::gfxShutdown();
    return EXIT_FAILURE;
  }
  app.lastFrameNs = platform::Window::nowNs();
  platform::runMainLoop(&app, frame, onExit);
  return app.exitCode;
}
