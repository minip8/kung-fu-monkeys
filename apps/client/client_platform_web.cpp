// The browser client's platform hooks (client_platform.hpp): the page's
// JavaScript through EM_JS. Compiled only into the Emscripten build; the
// guard is for CI's lint job, which runs clang-tidy over every .cpp with the
// native compile database (as engine/platform/src/main_loop.cpp's is).
#ifdef __EMSCRIPTEN__
#include "client_platform.hpp"

#include <emscripten/em_js.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

EM_JS_DEPS(kfm_platform, "$stringToNewUTF8,$UTF8ToString");
// localStorage can be missing or throw (privacy modes, sandboxed frames): the
// settings are then simply not remembered. (JavaScript: clang-format would
// break its operators.)
// clang-format off
EM_JS(char*, kfmLoadPrefJs, (const char* key), {
  try {
    const s = window.localStorage.getItem(UTF8ToString(key));
    return s === null ? 0 : stringToNewUTF8(s);
  } catch (e) {
    return 0;
  }
});
EM_JS(void, kfmSavePrefJs, (const char* key, const char* text), {
  try {
    window.localStorage.setItem(UTF8ToString(key), UTF8ToString(text));
  } catch (e) {
  }
});
// The page's address, for a room's invite link.
EM_JS(char*, kfmPageUrlJs, (), {
  try {
    return stringToNewUTF8(String(window.location.href));
  } catch (e) {
    return 0;
  }
});
// v0.43: Copy link and Share. Both need the page's transient activation,
// which the click (one frame earlier) gives; a refusal is silent.
EM_JS(int, kfmCopyTextJs, (const char* text), {
  try {
    if (!navigator.clipboard || !navigator.clipboard.writeText) return 0;
    navigator.clipboard.writeText(UTF8ToString(text)).catch(() => {});
    return 1;
  } catch (e) {
    return 0;
  }
});
EM_JS(int, kfmCanShareJs, (), {
  try {
    return typeof navigator.share === 'function' && navigator.maxTouchPoints > 0 ? 1 : 0;
  } catch (e) {
    return 0;
  }
});
EM_JS(int, kfmShareJs, (const char* title, const char* text, const char* url), {
  try {
    navigator.share({ title: UTF8ToString(title), text: UTF8ToString(text), url: UTF8ToString(url) })
        .catch(() => {});
    return 1;
  } catch (e) {
    return 0;
  }
});
// M7 step 6: the replay viewer's download. kfmFetchJs starts a CORS fetch
// without credentials (the ladder's presigned R2 URLs allow GET from the
// page's origin) and keeps the outcome in Module.kfmFetch; the frame polls
// it (kfmFetchPollJs: -1 still running or none, -2 failed, else the byte
// count), copies the bytes into a buffer it allocated (kfmFetchTakeJs) or
// takes the reason (kfmFetchErrorJs). A body past `cap` bytes is cut off
// and refused; leaving the screen aborts the fetch (kfmFetchCancelJs).
// The macro joins the body into one line: every statement ends in a ';'.
EM_JS(void, kfmFetchJs, (const char* url, int cap), {
  const state = { done: false, bytes: null, error: "", controller: null };
  Module['kfmFetch'] = state;
  const fail = (message) => {
    if (state.done) return;
    state.done = true;
    state.bytes = null;
    state.error = message;
  };
  try {
    const target = UTF8ToString(url);
    state.controller = typeof AbortController === 'function' ? new AbortController() : null;
    const options = { mode: 'cors', credentials: 'omit' };
    if (state.controller) options.signal = state.controller.signal;
    const tooLarge = 'the replay is larger than ' + (cap >> 20) + ' MiB';
    fetch(target, options)
        .then(async (response) => {
          if (!response.ok) {
            fail('the server answered HTTP ' + response.status +
                 (response.statusText ? ' ' + response.statusText : ""));
            return;
          }
          const declared = Number(response.headers.get('Content-Length') || 0);
          if (declared > cap) {
            fail(tooLarge);
            if (state.controller) state.controller.abort();
            return;
          }
          let bytes = null;
          if (response.body && typeof response.body.getReader === 'function') {
            const reader = response.body.getReader();
            const chunks = [];
            let total = 0;
            for (;;) {
              const part = await reader.read();
              if (part.done) break;
              total += part.value.length;
              if (total > cap) {
                reader.cancel().catch(() => {});
                fail(tooLarge);
                return;
              }
              chunks.push(part.value);
            }
            bytes = new Uint8Array(total);
            let at = 0;
            for (const chunk of chunks) {
              bytes.set(chunk, at);
              at += chunk.length;
            }
          } else {
            bytes = new Uint8Array(await response.arrayBuffer());
            if (bytes.length > cap) {
              fail(tooLarge);
              return;
            }
          }
          if (state.done) return;
          state.bytes = bytes;
          state.done = true;
        })
        .catch((e) => {
          fail('the download failed (' + (e && e.message ? e.message : String(e)) +
               '): the address is unreachable or does not allow this page (CORS)');
        });
  } catch (e) {
    fail('the download could not start: ' + String(e));
  }
});
EM_JS(int, kfmFetchPollJs, (), {
  const state = Module['kfmFetch'];
  if (!state || !state.done) return -1;
  return state.bytes ? state.bytes.length : -2;
});
EM_JS(int, kfmFetchActiveJs, (), {
  return Module['kfmFetch'] ? 1 : 0;
});
EM_JS(void, kfmFetchTakeJs, (unsigned char* out), {
  const state = Module['kfmFetch'];
  if (state && state.bytes && state.bytes.length > 0) HEAPU8.set(state.bytes, out);
  Module['kfmFetch'] = null;
});
EM_JS(char*, kfmFetchErrorJs, (), {
  const state = Module['kfmFetch'];
  Module['kfmFetch'] = null;
  return stringToNewUTF8(state && state.error ? state.error : 'unknown error');
});
EM_JS(void, kfmFetchCancelJs, (), {
  const state = Module['kfmFetch'];
  Module['kfmFetch'] = null;
  if (!state) return;
  state.done = true;
  state.bytes = null;
  if (state.controller) {
    try {
      state.controller.abort();
    } catch (e) {
    }
  }
});
// clang-format on

// localStorage keys: "kfm.settings", "kfm.guest_id".
[[nodiscard]] std::string prefKey(std::string_view name) {
  return "kfm." + std::string(name);
}

[[nodiscard]] std::string takeJsString(char* text) {
  if (text == nullptr) return {};
  std::string s(text);
  std::free(text);  // NOLINT(cppcoreguidelines-no-malloc): allocated by stringToNewUTF8
  return s;
}

[[nodiscard]] kfm::platform::Download pollFetch() {
  using kfm::platform::Download;
  if (kfmFetchActiveJs() == 0) return {};
  const int size = kfmFetchPollJs();
  if (size == -1) return Download{.state = Download::State::Pending, .bytes = {}, .error = {}};
  if (size < 0) {
    return Download{.state = Download::State::Failed,
                    .bytes = {},
                    .error = "cannot download it: " + takeJsString(kfmFetchErrorJs())};
  }
  Download d{.state = Download::State::Done,
             .bytes = std::vector<std::byte>(static_cast<std::size_t>(size)),
             .error = {}};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes for the JS copy
  kfmFetchTakeJs(reinterpret_cast<unsigned char*>(d.bytes.data()));
  return d;
}

}  // namespace

namespace kfm::client {

platform::PlatformHooks makePlatformHooks(bool persist) {
  platform::PlatformHooks hooks;
  if (persist) {
    hooks.prefs.load = [](std::string_view name) {
      return takeJsString(kfmLoadPrefJs(prefKey(name).c_str()));
    };
    hooks.prefs.save = [](std::string_view name, std::string_view text) {
      kfmSavePrefJs(prefKey(name).c_str(), std::string(text).c_str());
    };
  }
  hooks.share.copy = [](std::string_view text) {
    return kfmCopyTextJs(std::string(text).c_str()) != 0;
  };
  hooks.share.canShare = [] { return kfmCanShareJs() != 0; };
  hooks.share.share = [](std::string_view title, std::string_view text, std::string_view url) {
    return kfmShareJs(std::string(title).c_str(), std::string(text).c_str(),
                      std::string(url).c_str()) != 0;
  };
  hooks.download.start = [](std::string_view source, std::size_t maxBytes) {
    kfmFetchJs(std::string(source).c_str(), static_cast<int>(maxBytes));
  };
  hooks.download.poll = [] { return pollFetch(); };
  hooks.download.cancel = [] { kfmFetchCancelJs(); };
  hooks.pageUrl = takeJsString(kfmPageUrlJs());
  hooks.canQuit = false;     // a browser tab is closed by the browser
  hooks.localFiles = false;  // the page's in-memory copy: no hot reload
  return hooks;
}

}  // namespace kfm::client

#endif  // __EMSCRIPTEN__
