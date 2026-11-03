#include "kfm/platform/main_loop.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace kfm::platform {

#ifdef __EMSCRIPTEN__

namespace {

struct LoopState {
  void* user = nullptr;
  FrameFn frame = nullptr;
  ExitFn onExit = nullptr;
};

LoopState gLoop;

void browserFrame(void* arg) {
  auto* loop = static_cast<LoopState*>(arg);
  if (loop->frame(loop->user)) return;
  emscripten_cancel_main_loop();
  if (loop->onExit != nullptr) loop->onExit(loop->user);
}

}  // namespace

void runMainLoop(void* user, FrameFn frame, ExitFn onExit) {
  gLoop = LoopState{.user = user, .frame = frame, .onExit = onExit};
  // fps 0: follow requestAnimationFrame (§6.8). simulate_infinite_loop: never
  // return to the caller (main() is unwound by a JS throw, without running
  // destructors), so `user` must not point into main()'s stack frame.
  emscripten_set_main_loop_arg(browserFrame, &gLoop, 0, true);
}

#else

void runMainLoop(void* user, FrameFn frame, ExitFn onExit) {
  while (frame(user)) {
  }
  if (onExit != nullptr) onExit(user);
}

#endif

}  // namespace kfm::platform
