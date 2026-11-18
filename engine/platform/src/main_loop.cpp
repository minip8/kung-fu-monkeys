#include "kfm/platform/main_loop.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#endif

namespace kfm::platform {

#ifdef __EMSCRIPTEN__

namespace {

struct LoopState {
  void* user = nullptr;
  FrameFn frame = nullptr;
  ExitFn onExit = nullptr;
  BackgroundFn background = nullptr;
  double lastFrameMs = 0.0;  // emscripten_get_now() of the last animation frame
  bool running = false;
};

LoopState gLoop;

void browserFrame(void* arg) {
  auto* loop = static_cast<LoopState*>(arg);
  loop->lastFrameMs = emscripten_get_now();
  const bool more = loop->frame(loop->user);
  // From the frame's end: a long frame is not an absence of frames.
  loop->lastFrameMs = emscripten_get_now();
  if (more) return;
  loop->running = false;
  emscripten_cancel_main_loop();
  if (loop->onExit != nullptr) loop->onExit(loop->user);
}

// v0.92: the background pump (see the header). The Worker's source is a
// string turned into a Blob URL, so no extra file ships with the page. Its
// messages carry nothing; the main thread asks the page whether it is hidden.
// (JavaScript: clang-format would break its operators.)
// clang-format off
EM_JS(void, kfmStartBackgroundPumpJs, (int periodMs), {
  const wake = () => {
    try {
      _kfm_platform_background_wake(document.visibilityState === 'hidden' ? 1 : 0);
    } catch (e) {
      console.error('kfm background pump:', e);
    }
  };
  try {
    const source = 'let timer = 0; onmessage = (e) => { clearInterval(timer); ' +
                   'timer = setInterval(() => postMessage(0), e.data); };';
    const url = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
    const worker = new Worker(url);  // the URL is kept: the script loads later
    worker.onmessage = wake;
    worker.onerror = (e) => {
      console.warn('kfm background pump: worker failed, using a page timer', e);
      setInterval(wake, periodMs);
    };
    worker.postMessage(periodMs);
    Module['kfmBackgroundWorker'] = worker;
  } catch (e) {
    console.warn('kfm background pump: no worker, using a page timer', e);
    setInterval(wake, periodMs);
  }
});
// clang-format on

}  // namespace

void runMainLoop(void* user, FrameFn frame, ExitFn onExit, BackgroundFn background) {
  gLoop = LoopState{.user = user,
                    .frame = frame,
                    .onExit = onExit,
                    .background = background,
                    .lastFrameMs = emscripten_get_now(),
                    .running = true};
  if (background != nullptr) kfmStartBackgroundPumpJs(kBackgroundPumpMs);
  // fps 0: follow requestAnimationFrame (§6.8). simulate_infinite_loop: never
  // return to the caller (main() is unwound by a JS throw, without running
  // destructors), so `user` must not point into main()'s stack frame.
  emscripten_set_main_loop_arg(browserFrame, &gLoop, 0, true);
}

#else

void runMainLoop(void* user, FrameFn frame, ExitFn onExit, BackgroundFn /*background*/) {
  while (frame(user)) {
  }
  if (onExit != nullptr) onExit(user);
}

#endif

}  // namespace kfm::platform

#ifdef __EMSCRIPTEN__
// Called by the background pump's timer (above), on the main thread.
extern "C" EMSCRIPTEN_KEEPALIVE void kfm_platform_background_wake(int hidden) {
  using kfm::platform::gLoop;
  if (!gLoop.running || gLoop.background == nullptr) return;
  const double idleMs = emscripten_get_now() - gLoop.lastFrameMs;
  if (hidden == 0 && idleMs < kfm::platform::kBackgroundAfterMs) return;
  gLoop.background(gLoop.user);
}
#endif
