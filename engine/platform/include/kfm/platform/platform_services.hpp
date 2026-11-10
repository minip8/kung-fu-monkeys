// IPlatformServices: everything the client needs that differs between the
// browser and a native build (architecture §13.4, M6a v0.37 §13.5). Only
// the client uses it; Steam's implementation (M8) would add invites and
// presence behind the same seam. Besides ITransport, it is the only place
// that knows which platform the client runs on.
//
//   NullPlatformServices   tests: no identity, no lobby, no storage.
//   LocalPlatformServices  native and web: the platform-free half (the
//                          guest id, the lobby URL, the share gating) over
//                          injected PlatformHooks; the client builds the
//                          hooks per platform (apps/client/
//                          client_platform_native.cpp: SDL; _web.cpp: the
//                          browser through EM_JS).
//
// Preferences: named texts ("settings", "guest_id") kept in SDL's pref dir
// natively (<name>.txt) and in localStorage in the browser (kfm.<name>);
// none under --no-settings (the hooks are left empty).
//
// The guest id is 32 lower-case hex digits (128 bits), kept as the
// "guest_id" preference. It is not sent anywhere yet: it exists so the bot
// ladder (M7) can bind accounts to it.
//
// Sharing (v0.43, M6b): copyText() puts text on the system clipboard
// (natively SDL's, in the browser navigator.clipboard.writeText) and
// share() hands a link to the device's share sheet (the Web Share API,
// navigator.share), offered only where canShare() says so: in a browser
// that has navigator.share on a touch device.
//
// Downloads (the replay viewer, M7 step 6): startDownload() asks for the
// bytes at `source` (natively a file path, read at once; in the browser a
// CORS fetch without credentials) and pollDownload() reports the outcome
// once: Done with the bytes or Failed with a reason a player can read; at
// most `maxBytes` are taken, a bigger source fails. One download at a time.
//
// The page (pageUrl(): the browser's address; natively the build's page URL
// or empty), whether the app may close itself (canQuit(): not a browser
// tab) and whether the files it reads are the user's own and may change
// while it runs (localFiles(): natively, for hot reload; the browser's are
// the page's in-memory copy) complete the seam.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::platform {

// A download's outcome (pollDownload()).
struct Download {
  enum class State : uint8_t { None = 0, Pending, Done, Failed };
  State state{State::None};
  std::vector<std::byte> bytes;  // Done
  std::string error;             // Failed: why, for the player
};

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

  // A stored preference text, empty when there is none (or nothing is kept).
  [[nodiscard]] virtual std::string loadPref(std::string_view name) = 0;
  // Stores it; a failure is reported on stderr at most, never fatal.
  virtual void savePref(std::string_view name, std::string_view text) = 0;
  // The page the client runs in (for invite links), or empty.
  [[nodiscard]] virtual std::string pageUrl() = 0;
  // Whether the app may close itself (a Quit item, Escape on the menu).
  [[nodiscard]] virtual bool canQuit() = 0;
  // Whether the files the app reads may change while it runs (hot reload).
  [[nodiscard]] virtual bool localFiles() = 0;
  // Starts a download (replacing one in flight); see the top of the file.
  virtual void startDownload(std::string_view source, std::size_t maxBytes) = 0;
  // Pending while it runs, then Done or Failed once, then None.
  [[nodiscard]] virtual Download pollDownload() = 0;
  // Drops the download in flight, if any.
  virtual void cancelDownload() = 0;
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
  [[nodiscard]] std::string loadPref(std::string_view /*name*/) override { return {}; }
  void savePref(std::string_view /*name*/, std::string_view /*text*/) override {}
  [[nodiscard]] std::string pageUrl() override { return {}; }
  [[nodiscard]] bool canQuit() override { return true; }
  [[nodiscard]] bool localFiles() override { return false; }
  void startDownload(std::string_view source, std::size_t maxBytes) override;
  [[nodiscard]] Download pollDownload() override;
  void cancelDownload() override { download_ = {}; }

 private:
  Download download_;
};

// Where LocalPlatformServices keeps preference texts. load(name) returns the
// stored text (empty when none); save(name, text) stores it and may fail
// silently (a guest id is then made again next run). Both missing: nothing
// is kept (--no-settings).
struct PrefStore {
  std::function<std::string(std::string_view)> load;
  std::function<void(std::string_view, std::string_view)> save;
};

// How LocalPlatformServices reaches the clipboard and the share sheet
// (v0.43). A missing function means the platform has none: copyText() and
// share() return false, canShare() false.
struct ShareHooks {
  std::function<bool(std::string_view)> copy;
  std::function<bool()> canShare;
  std::function<bool(std::string_view, std::string_view, std::string_view)> share;
};

// How LocalPlatformServices downloads (the replay viewer). start(source,
// maxBytes) begins one, poll() reports it as IPlatformServices::
// pollDownload() does, cancel() drops it. Missing: every download fails.
struct DownloadHooks {
  std::function<void(std::string_view, std::size_t)> start;
  std::function<Download()> poll;
  std::function<void()> cancel;
};

// Everything platform-specific LocalPlatformServices reaches through.
struct PlatformHooks {
  PrefStore prefs;
  ShareHooks share;
  DownloadHooks download;
  std::string pageUrl;     // pageUrl()
  bool canQuit{true};      // canQuit()
  bool localFiles{false};  // localFiles()
};

// Whether `id` is a guest id (32 lower-case hex digits).
[[nodiscard]] bool isGuestId(std::string_view id);
// A guest id from 128 bits of entropy (two splitmix64 outputs).
[[nodiscard]] std::string makeGuestId(uint64_t entropyA, uint64_t entropyB);

class LocalPlatformServices final : public IPlatformServices {
 public:
  // `entropyA`/`entropyB` seed a new guest id when the store has none (the
  // app passes std::random_device and the clock).
  LocalPlatformServices(PlatformHooks hooks, std::string lobbyUrl, uint64_t entropyA,
                        uint64_t entropyB);

  // The stored id; the first call without a valid one makes and saves one.
  [[nodiscard]] std::string guestId() override;
  [[nodiscard]] std::string lobbyUrl() override { return lobbyUrl_; }
  bool copyText(std::string_view text) override;
  [[nodiscard]] bool canShare() override;
  bool share(std::string_view title, std::string_view text, std::string_view url) override;
  [[nodiscard]] std::string loadPref(std::string_view name) override;
  void savePref(std::string_view name, std::string_view text) override;
  [[nodiscard]] std::string pageUrl() override { return hooks_.pageUrl; }
  [[nodiscard]] bool canQuit() override { return hooks_.canQuit; }
  [[nodiscard]] bool localFiles() override { return hooks_.localFiles; }
  void startDownload(std::string_view source, std::size_t maxBytes) override;
  [[nodiscard]] Download pollDownload() override;
  void cancelDownload() override;

 private:
  PlatformHooks hooks_;
  Download refused_;  // a download without hooks: Failed once
  std::string lobbyUrl_;
  uint64_t entropyA_;
  uint64_t entropyB_;
  std::string id_;  // cached once known
};

}  // namespace kfm::platform
