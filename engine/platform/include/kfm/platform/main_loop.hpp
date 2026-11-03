// Main loop that works natively and in the browser (architecture §6.8, §13).
//
// Natively: `while (frame(user)) {}` then onExit(user), then returns.
// Emscripten: the browser drives frame() from requestAnimationFrame
// (emscripten_set_main_loop_arg with fps 0). When frame() returns false the
// loop is cancelled and onExit(user) runs; runMainLoop itself never returns
// there (simulate_infinite_loop), so put all cleanup in onExit, not after the
// call, and keep `user` in static or heap storage, never on main()'s stack.
#pragma once

namespace kfm::platform {

using FrameFn = bool (*)(void* user);
using ExitFn = void (*)(void* user);

void runMainLoop(void* user, FrameFn frame, ExitFn onExit = nullptr);

}  // namespace kfm::platform
