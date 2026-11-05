// IPlatformServices: the platform's identity and session directory
// (architecture §13.4, M6a v0.37 §13.5). Only the client uses it; Steam's
// implementation (M7) would add invites and presence behind the same seam.
//
//   NullPlatformServices   tests: no identity, no lobby.
//   LocalPlatformServices  native and web: a guest id made once and kept in
//                          the client's preference storage (next to the
//                          lobby settings: SDL's pref dir natively, the
//                          browser's localStorage), and the lobby URL of the
//                          options as the session directory.
//
// The guest id is 32 lower-case hex digits (128 bits). It is not sent
// anywhere yet: it exists so the bot ladder (M7) can bind accounts to it.
// Storage is injected (GuestIdStore) so this stays SDL-free and headless.
//
// Sharing (v0.43, M6b): copyText() puts text on the system clipboard
// (natively SDL's, in the browser navigator.clipboard.writeText) and
// share() hands a link to the device's share sheet (the Web Share API,
// navigator.share), offered only where canShare() says so: in a browser
// that has navigator.share on a touch device. The app injects both
// (ShareHooks) so this target stays SDL- and Emscripten-free; Null has
// neither, and tests inject recording fakes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace kfm::platform {

class IPlatformServices {
 public:
  IPlatformServices() = default;
  IPlatformServices(const IPlatformServices&) = delete;
  IPlatformServices& operator=(const IPlatformServices&) = delete;
  IPlatformServices(IPlatformServices&&) = delete;
  IPlatformServices& operator=(IPlatformServices&&) = delete;
  virtual ~IPlatformServices() = default;

  // This player's identity: a stable id, or empty when the platform has none.
  [[nodiscard]] virtual std::string guestId() = 0;
  // The session directory: the lobby's base URL, or empty (no rooms).
  [[nodiscard]] virtual std::string lobbyUrl() = 0;
  // v0.43: the clipboard. False when the platform has none or refused.
  virtual bool copyText(std::string_view text) = 0;
  // v0.43: whether share() opens a share sheet here (a browser with the
  // Web Share API on a touch device).
  [[nodiscard]] virtual bool canShare() = 0;
  // v0.43: offers `url` (titled `title`, described by `text`) to the share
  // sheet. False when canShare() is false or the platform refused.
  virtual bool share(std::string_view title, std::string_view text, std::string_view url) = 0;
};

class NullPlatformServices final : public IPlatformServices {
 public:
  [[nodiscard]] std::string guestId() override { return {}; }
  [[nodiscard]] std::string lobbyUrl() override { return {}; }
  bool copyText(std::string_view /*text*/) override { return false; }
  [[nodiscard]] bool canShare() override { return false; }
  bool share(std::string_view /*title*/, std::string_view /*text*/,
             std::string_view /*url*/) override {
    return false;
  }
};

// Where LocalPlatformServices keeps the guest id. load() returns the stored
// text (empty when none); save() stores it and may fail silently (the id is
// then made again next run).
struct GuestIdStore {
  std::function<std::string()> load;
  std::function<void(std::string_view)> save;
};

// How LocalPlatformServices reaches the clipboard and the share sheet
// (v0.43). A missing function means the platform has none: copyText() and
// share() return false, canShare() false.
struct ShareHooks {
  std::function<bool(std::string_view)> copy;
  std::function<bool()> canShare;
  std::function<bool(std::string_view, std::string_view, std::string_view)> share;
};

// Whether `id` is a guest id (32 lower-case hex digits).
[[nodiscard]] bool isGuestId(std::string_view id);
// A guest id from 128 bits of entropy (two splitmix64 outputs).
[[nodiscard]] std::string makeGuestId(uint64_t entropyA, uint64_t entropyB);

class LocalPlatformServices final : public IPlatformServices {
 public:
  // `entropyA`/`entropyB` seed a new id when the store has none (the app
  // passes std::random_device and the clock).
  LocalPlatformServices(GuestIdStore store, std::string lobbyUrl, uint64_t entropyA,
                        uint64_t entropyB, ShareHooks share = {});

  // The stored id; the first call without a valid one makes and saves one.
  [[nodiscard]] std::string guestId() override;
  [[nodiscard]] std::string lobbyUrl() override { return lobbyUrl_; }
  bool copyText(std::string_view text) override;
  [[nodiscard]] bool canShare() override;
  bool share(std::string_view title, std::string_view text, std::string_view url) override;

 private:
  GuestIdStore store_;
  ShareHooks share_;
  std::string lobbyUrl_;
  uint64_t entropyA_;
  uint64_t entropyB_;
  std::string id_;  // cached once known
};

}  // namespace kfm::platform
