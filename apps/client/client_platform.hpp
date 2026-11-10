// The client's IPlatformServices per platform (architecture §13.4): the
// hooks LocalPlatformServices reaches the OS through. CMake compiles one of
// two files into kfm_client (the web one keeps an __EMSCRIPTEN__ guard only
// for the native lint job):
//
//   client_platform.cpp         shared: the guest id's entropy, the services.
//   client_platform_native.cpp  SDL: preferences as <name>.txt in SDL's pref
//                               dir, SDL's clipboard, no share sheet, a
//                               download reads a local file at once, the
//                               page URL is the build's KFM_PAGE_URL, Quit,
//                               hot reload.
//   client_platform_web.cpp     the browser through EM_JS: localStorage
//                               (kfm.<name>), navigator.clipboard and
//                               navigator.share (touch devices), a CORS
//                               fetch without credentials, the page's
//                               window.location.href; no Quit, no hot reload.
//
// Everything else in the client stays platform-free; main.cpp asks the
// services.
#pragma once

#include "kfm/platform/platform_services.hpp"

#include <memory>
#include <string>

namespace kfm::client {

struct ClientPlatformConfig {
  std::string lobbyUrl;  // the session directory (ClientOptions::lobby)
  bool persist{true};    // false (--no-settings): no preference is read or kept
};

// The services for this build's platform. The guest id's entropy comes from
// std::random_device and the clock.
[[nodiscard]] std::unique_ptr<platform::IPlatformServices> makeClientPlatformServices(
    const ClientPlatformConfig& config);

// This platform's hooks (client_platform_native.cpp or _web.cpp); without
// `persist` the preference hooks are left empty.
[[nodiscard]] platform::PlatformHooks makePlatformHooks(bool persist);

}  // namespace kfm::client
