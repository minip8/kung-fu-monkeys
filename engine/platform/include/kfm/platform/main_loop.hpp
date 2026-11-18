// Main loop that works natively and in the browser (architecture §6.8, §13).
//
// Natively: `while (frame(user)) {}` then onExit(user), then returns.
// Emscripten: the browser drives frame() from requestAnimationFrame
// (emscripten_set_main_loop_arg with fps 0). When frame() returns false the
// loop is cancelled and onExit(user) runs; runMainLoop itself never returns
// there (simulate_infinite_loop), so put all cleanup in onExit, not after the
// call, and keep `user` in static or heap storage, never on main()'s stack.
//
// The background pump (v0.92, architecture §6.5): a browser stops
// requestAnimationFrame for a hidden page (another tab in front, the window
// minimised or covered), so nothing would run and the game's network link
// would time out. With `background` set, a dedicated Worker that does nothing
// but keep time posts a message every kBackgroundPumpMs (a Worker's timers are
// not throttled like a hidden page's, which run at most once a second and,
// in Chrome, once a minute after five minutes), and the main thread calls
// background(user) for each one while the page is hidden or no animation
// frame has run for kBackgroundAfterMs. Everything still runs on the main
// thread: the Worker only wakes it. If the Worker cannot be created, a
// main-thread interval does the same at whatever rate the browser allows.
// Natively the loop never stops, and background() is never called.
#pragma once

namespace kfm::platform {

using FrameFn = bool (*)(void* user);
using ExitFn = void (*)(void* user);
using BackgroundFn = void (*)(void* user);

// v0.92: the background pump's period, and how long without an animation
// frame counts as being in the background on a page that says it is visible.
inline constexpr int kBackgroundPumpMs = 50;
inline constexpr int kBackgroundAfterMs = 2000;

void runMainLoop(void* user, FrameFn frame, ExitFn onExit = nullptr,
                 BackgroundFn background = nullptr);

}  // namespace kfm::platform
