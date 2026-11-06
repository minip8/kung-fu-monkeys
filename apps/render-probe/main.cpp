// kfm_render_probe: drives a scripted match through the renderer (architecture
// §7, §11 M2) so the 3D presentation can be checked without a human playing.
// The sim runs the scripted input generator (the same one as `kfm_replay
// record-scripted`); every frame steps K ticks, builds the RenderState with
// alpha = 1 from the newest tick and draws it with the fixed arena camera.
//
//   kfm_render_probe [--players N] [--seed S] [--input-seed I] [--ticks-per-frame K]
//                    [--width W] [--height H] [--exit-after-frames N]
//                    [--screenshot out.ppm] [--hide-ui] [--exit-on WHAT] [--capsules]
//                    [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]
//
// --exit-after-frames N renders N frames and exits 0 (rendering smoke test).
// --screenshot path     writes the last rendered frame as binary PPM (native only;
//                       with no --exit-after-frames it is taken when the probe quits).
// --hide-ui             no Dear ImGui overlay (clean screenshots).
// --exit-on WHAT        ends on the first frame showing WHAT, one of dash-ring,
//                       perfect-catch, auto-catch, hit, throw, elimination (effects)
//                       or catch-window (a player with the PerfectDash flag);
//                       --exit-after-frames still bounds the run.
// --capsules            draw players as capsules instead of the skinned model
//                       (also the fallback when content/models/test_rig.glb is missing).
// --content DIR         content directory (default: content/ next to the
//                       executable, else the embedded copy); --arena / --mode /
//                       --loadout pick the match's arena, mode and loadout by name.
// A match that ends is replaced by a fresh one (next seed) after a short pause.
#include "kfm/content/app_content.hpp"
#include "kfm/core/rng.hpp"
#include "kfm/platform/debug_ui.hpp"
#include "kfm/platform/gfx.hpp"
#include "kfm/platform/main_loop.hpp"
#include "kfm/platform/window.hpp"
#include "kfm/presentation/builder.hpp"
#include "kfm/presentation/camera.hpp"
#include "kfm/presentation/palette.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/renderer.hpp"
#include "kfm/presentation/theme.hpp"
#include "kfm/sim/scripted_inputs.hpp"
#include "kfm/sim/sim.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keycode.h>
#include <imgui.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace kfm;

constexpr uint32_t kMaxTicksPerFrame = 64;
constexpr uint32_t kMatchEndPauseTicks = 180;  // show the final state before restarting

struct Options {
  uint32_t players = 6;
  uint64_t seed = 0x4B464D00ULL;
  uint64_t inputSeed = 0x5C121DULL;
  uint32_t ticksPerFrame = 1;
  int width = 1280;
  int height = 720;
  uint64_t exitAfterFrames = 0;  // 0: run until quit
  const char* screenshotPath = nullptr;
  bool hideUi = false;
  bool capsules = false;
  presentation::ThemeChoice theme = presentation::ThemeChoice::Auto;  // v0.52
  int exitOn = -1;  // -1: none; < kExitOnWindow: an EffectKind; kExitOnWindow: PerfectDash
  std::string_view contentDir;  // empty: next to the executable, else embedded
  std::string_view arena;       // names; empty = the first one
  std::string_view mode;
  std::string_view loadout;
};

constexpr int kExitOnWindow = 100;

struct ExitOnName {
  std::string_view name;
  int value;
};
constexpr std::array<ExitOnName, 7> kExitOnNames = {{
    {"dash-ring", static_cast<int>(presentation::EffectKind::DashRing)},
    {"perfect-catch", static_cast<int>(presentation::EffectKind::PerfectCatchFlash)},
    {"auto-catch", static_cast<int>(presentation::EffectKind::AutoCatchFlash)},
    {"hit", static_cast<int>(presentation::EffectKind::HitFlash)},
    {"throw", static_cast<int>(presentation::EffectKind::ThrowPuff)},
    {"elimination", static_cast<int>(presentation::EffectKind::EliminationBurst)},
    {"catch-window", kExitOnWindow},
}};

struct App {
  Options options;
  content::LoadedContent content;
  sim::MatchConfig config{};  // of the current match
  platform::Window window;
  presentation::Renderer renderer;
  presentation::PresentationState presentation{};
  presentation::RenderState renderState{};
  sim::WorldState prev{};
  sim::WorldState curr{};
  Rng inputs{};
  sim::EventSink sink{};
  std::vector<sim::SimEvent> frameEvents;  // reserved once; never grows per frame
  uint32_t matches = 1;
  uint64_t frames = 0;
  uint64_t lastFrameNs = 0;
  bool quit = false;
  int exitCode = EXIT_SUCCESS;
};

// Static storage: under Emscripten main() is unwound once the loop starts.
// The sim's default member initialisers are not declared noexcept, but they
// are plain value initialisers that cannot throw.
App gApp;  // NOLINT(bugprone-throwing-static-initialization)

constexpr const char* kUsage =
    "usage: kfm_render_probe [--players N] [--seed S] [--input-seed I] [--ticks-per-frame K]\n"
    "                        [--width W] [--height H] [--exit-after-frames N]\n"
    "                        [--screenshot out.ppm] [--hide-ui] [--exit-on WHAT] [--capsules]\n"
    "                        [--content DIR] [--arena NAME] [--mode NAME] [--loadout NAME]\n"
    "                        [--theme auto|jungle|temple|beach]\n";

template <typename T>
bool parseNumber(std::string_view v, T& out) {
  const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
  return ec == std::errc{} && end == v.data() + v.size();
}

bool parseArgs(int argc, const char* const* argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool hasValue = i + 1 < argc;
    bool ok = true;
    if (arg == "--hide-ui") {
      o.hideUi = true;
    } else if (arg == "--capsules") {
      o.capsules = true;
    } else if (arg == "--theme" && hasValue) {
      const auto theme = presentation::parseThemeChoice(argv[++i]);
      if (!theme) return false;
      o.theme = *theme;
    } else if (arg == "--exit-on" && hasValue) {
      const std::string_view what = argv[++i];
      ok = false;
      for (const ExitOnName& n : kExitOnNames) {
        if (n.name == what) {
          o.exitOn = n.value;
          ok = true;
        }
      }
    } else if (arg == "--content" && hasValue) {
      o.contentDir = argv[++i];
    } else if (arg == "--arena" && hasValue) {
      o.arena = argv[++i];
    } else if (arg == "--mode" && hasValue) {
      o.mode = argv[++i];
    } else if (arg == "--loadout" && hasValue) {
      o.loadout = argv[++i];
    } else if (arg == "--screenshot" && hasValue) {
      o.screenshotPath = argv[++i];
    } else if (arg == "--players" && hasValue) {
      ok = parseNumber(argv[++i], o.players) && o.players >= 1 && o.players <= sim::kMaxPlayers;
    } else if (arg == "--seed" && hasValue) {
      ok = parseNumber(argv[++i], o.seed);
    } else if (arg == "--input-seed" && hasValue) {
      ok = parseNumber(argv[++i], o.inputSeed);
    } else if (arg == "--ticks-per-frame" && hasValue) {
      ok = parseNumber(argv[++i], o.ticksPerFrame) && o.ticksPerFrame <= kMaxTicksPerFrame;
    } else if (arg == "--width" && hasValue) {
      ok = parseNumber(argv[++i], o.width) && o.width >= 64;
    } else if (arg == "--height" && hasValue) {
      ok = parseNumber(argv[++i], o.height) && o.height >= 64;
    } else if (arg == "--exit-after-frames" && hasValue) {
      ok = parseNumber(argv[++i], o.exitAfterFrames) && o.exitAfterFrames > 0;
    } else {
      std::fprintf(stderr, "error: unknown or incomplete argument '%s'\n", argv[i]);
      return false;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value '%s' for %s\n", argv[i], argv[i - 1]);
      return false;
    }
  }
  return true;
}

// The rigged character model: generated by the build next to the executable
// natively (kfm_use_test_rig), embedded at a fixed path under Emscripten.
[[nodiscard]] std::string characterModelPath() {
#ifdef __EMSCRIPTEN__
  return "/content/models/test_rig.glb";
#else
  const char* base = SDL_GetBasePath();  // owned by SDL, ends with a separator
  return std::string(base != nullptr ? base : "") + "content/models/test_rig.glb";
#endif
}

// Match `match` (1-based): the next seed, the same arena, mode and loadout.
[[nodiscard]] sim::MatchConfig matchConfig(const App& app, uint32_t match) {
  sim::MatchConfig c = app.config;
  c.seed = app.options.seed + match - 1;
  c.players = static_cast<sim::PlayerMask>((1u << app.options.players) - 1u);
  return c;
}

[[nodiscard]] sim::MatchContent matchContent(const App& app) {
  return sim::matchContent(app.content.db, app.config);
}

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

// Steps the scripted match K ticks; every tick's events land in frameEvents.
void stepSim(App& app) {
  const sim::MatchContent content = matchContent(app);
  app.frameEvents.clear();
  for (uint32_t k = 0; k < app.options.ticksPerFrame; ++k) {
    if (app.curr.rules.phase == sim::MatchPhase::MatchEnded &&
        app.curr.rules.ticksInPhase >= kMatchEndPauseTicks) {
      ++app.matches;
      app.config = matchConfig(app, app.matches);
      app.curr = sim::initialWorld(app.config);
      app.presentation = {};
    }
    app.prev = app.curr;
    const sim::InputFrame f = sim::scriptedFrame(app.inputs, app.curr, content);
    app.sink.clear();
    sim::step(app.curr, f, content, app.sink);
    app.frameEvents.insert(app.frameEvents.end(), app.sink.events.begin(),
                           app.sink.events.begin() + app.sink.count);
  }
}

[[nodiscard]] bool showsExitOn(const presentation::RenderState& rs, int exitOn) {
  if (exitOn < 0) return false;
  if (exitOn == kExitOnWindow) {
    for (uint16_t i = 0; i < rs.playerCount; ++i) {
      if (rs.players[i].visible && rs.players[i].has(presentation::PlayerFlag::PerfectDash))
        return true;
    }
    return false;
  }
  for (uint16_t i = 0; i < rs.effectCount; ++i) {
    if (static_cast<int>(rs.effects[i].kind) == exitOn) return true;
  }
  return false;
}

bool frame(void* user) {
  auto* app = static_cast<App*>(user);
  if (!app->window.pollEvents(onEvent, app)) app->quit = true;

  const std::uint64_t now = platform::Window::nowNs();
  const double dt = static_cast<double>(now - app->lastFrameNs) * 1e-9;
  app->lastFrameNs = now;

  stepSim(*app);
  // Presentation time advances with sim time, so screenshots are reproducible.
  const float simDt = static_cast<float>(app->options.ticksPerFrame) / 60.0f;
  const sim::MatchContent content = matchContent(*app);
  presentation::buildRenderState(app->presentation, app->prev, app->curr, 1.0f, app->frameEvents,
                                 content, simDt, app->renderState);

  const platform::DrawableSize size = app->window.drawableSize();
  const presentation::Camera camera = presentation::makeArenaCamera(
      content.arena, static_cast<float>(size.width), static_cast<float>(size.height));

  platform::debugUiNewFrame(size.width, size.height, dt, app->window.pixelDensity());
  if (!app->options.hideUi) {
    ImGui::SetNextWindowPos(ImVec2(16.0F, 16.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Render probe");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("fps %.1f", static_cast<double>(io.Framerate));
    ImGui::Text("tick %u  round %u  match %u", app->curr.tick.value, app->curr.rules.round,
                app->matches);
    ImGui::Text("draw calls %u  triangles %u", app->renderer.stats().drawCalls,
                app->renderer.stats().triangles);
    ImGui::Text("players %u  bananas %u  pickups %u  effects %u", app->renderState.playerCount,
                app->renderState.bananaCount, app->renderState.pickupCount,
                app->renderState.effectCount);
    ImGui::Text("drawable %d x %d", size.width, size.height);
    bool capsules = app->renderer.forceCapsules();
    if (app->renderer.characterModelLoaded() && ImGui::Checkbox("Capsules", &capsules))
      app->renderer.setForceCapsules(capsules);
    if (ImGui::Button("Quit")) app->quit = true;
    ImGui::End();
  }
  platform::debugUiFlush();

  // v0.49: the arena look's canopy around the arena.
  const presentation::Color bg = app->renderer.look().background;
  platform::beginDefaultPass({.r = bg.r, .g = bg.g, .b = bg.b, .a = 1.0F}, size.width, size.height);
  app->renderer.draw(app->renderState, camera, size.width, size.height);
  platform::debugUiDraw();
  platform::endPass();

  ++app->frames;
  const bool lastFrame =
      app->quit ||
      (app->options.exitAfterFrames != 0 && app->frames >= app->options.exitAfterFrames) ||
      showsExitOn(app->renderState, app->options.exitOn);
  if (lastFrame) {
    std::printf("kfm_render_probe: %llu frames, tick %u, %u draw calls, %u triangles\n",
                static_cast<unsigned long long>(app->frames), app->curr.tick.value,
                app->renderer.stats().drawCalls, app->renderer.stats().triangles);
  }
  if (lastFrame && app->options.screenshotPath != nullptr &&
      !writePpm(app->options.screenshotPath, size.width, size.height)) {
    app->exitCode = EXIT_FAILURE;
  }
  app->window.swap();
  return !lastFrame;
}

void onExit(void* user) {
  auto* app = static_cast<App*>(user);
  app->renderer.shutdown();
  platform::debugUiShutdown();
  platform::gfxShutdown();
  app->window.shutdown();
}

}  // namespace

int main(int argc, char** argv) {  // NOLINT(misc-const-correctness): main signature
  App& app = gApp;
  if (!parseArgs(argc, argv, app.options)) {
    std::fputs(kUsage, stderr);
    return EXIT_FAILURE;
  }
  {
    content::Diagnostics diag;
    const char* base = SDL_GetBasePath();  // owned by SDL
    const bool loaded = content::loadAppContent(
        app.options.contentDir,
        base != nullptr ? std::filesystem::path(base) : std::filesystem::path{}, app.content, diag);
    if (!diag.items().empty()) std::fprintf(stderr, "%s", diag.format().c_str());
    std::string error;
    if (!loaded ||
        !content::resolveMatchNames(app.content.db, app.options.arena, app.options.mode, app.config,
                                    error) ||
        !content::resolveLoadoutName(app.content.db, app.options.loadout, app.config, error)) {
      std::fprintf(stderr, "error: %s\n", loaded ? error.c_str() : "the content failed to load");
      return EXIT_FAILURE;
    }
    std::printf("kfm_render_probe: %s, arena %s, mode %s, loadout %s\n",
                app.content.describe().c_str(),
                app.content.db.arenaAt(app.config.arena).name.cStr(),
                app.content.db.modeAt(app.config.mode).name.cStr(),
                app.content.db.loadoutAt(app.config.loadout).name.cStr());
  }
  if (!app.window.init({.title = "Kung Fu Monkeys: render probe",
                        .width = app.options.width,
                        .height = app.options.height,
                        .vsync = true})) {
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
  app.renderer.setThemeChoice(app.options.theme);
  if (!app.renderer.init(matchContent(app))) {
    std::fprintf(stderr, "error: the renderer failed to initialise\n");
    platform::debugUiShutdown();
    platform::gfxShutdown();
    return EXIT_FAILURE;
  }
  if (!app.options.capsules) {
    const std::string modelPath = characterModelPath();
    if (!app.renderer.loadCharacterModel(modelPath.c_str())) {
      std::fprintf(stderr, "warning: drawing capsules: %s\n",
                   app.renderer.characterModelError().c_str());
    }
  }
  app.config = matchConfig(app, app.matches);
  app.curr = sim::initialWorld(app.config);
  app.prev = app.curr;
  app.inputs = Rng::seeded(app.options.inputSeed);
  app.frameEvents.reserve(static_cast<std::size_t>(kMaxTicksPerFrame) * sim::kMaxEventsPerTick);
  app.lastFrameNs = platform::Window::nowNs();
  platform::runMainLoop(&app, frame, onExit);
  return app.exitCode;
}
