// IPlatformServices (M6a, architecture §13.4/§13.5): the null services and
// LocalPlatformServices' guest id, made once and kept in an injected store
// (the client's settings storage).
#include "kfm/platform/platform_services.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using kfm::platform::GuestIdStore;
using kfm::platform::isGuestId;
using kfm::platform::LocalPlatformServices;
using kfm::platform::makeGuestId;
using kfm::platform::NullPlatformServices;

namespace {

// An in-memory stand-in for the settings storage (file / localStorage).
struct MemoryStore {
  std::string text;
  int saves{0};
  [[nodiscard]] GuestIdStore store() {
    return GuestIdStore{.load = [this] { return text; },
                        .save =
                            [this](std::string_view t) {
                              text = t;
                              ++saves;
                            }};
  }
};

}  // namespace

TEST_CASE("null platform services have no identity and no lobby", "[platform][services]") {
  NullPlatformServices null;
  CHECK(null.guestId().empty());
  CHECK(null.lobbyUrl().empty());
}

TEST_CASE("guest ids are 32 lower-case hex digits, distinct per entropy", "[platform][services]") {
  const std::string a = makeGuestId(1, 2);
  CHECK(isGuestId(a));
  CHECK(a == makeGuestId(1, 2));
  CHECK(a != makeGuestId(1, 3));
  CHECK(a != makeGuestId(2, 2));
  CHECK_FALSE(isGuestId(""));
  CHECK_FALSE(isGuestId("0123456789abcdef0123456789abcde"));   // 31
  CHECK_FALSE(isGuestId("0123456789ABCDEF0123456789abcdef"));  // upper case
  CHECK_FALSE(isGuestId("0123456789abcdef0123456789abcdeg"));  // not hex
  CHECK(isGuestId("0123456789abcdef0123456789abcdef"));
}

TEST_CASE("the guest id is made once, saved, and read back on the next run",
          "[platform][services]") {
  MemoryStore disk;
  std::string first;
  {
    LocalPlatformServices run1(disk.store(), "ws://127.0.0.1:8080", 11, 22);
    first = run1.guestId();
    CHECK(isGuestId(first));
    CHECK(run1.guestId() == first);  // cached: one save
    CHECK(disk.saves == 1);
    CHECK(disk.text == first);
    CHECK(run1.lobbyUrl() == "ws://127.0.0.1:8080");
  }
  // Another run with other entropy keeps the stored id and saves nothing.
  LocalPlatformServices run2(disk.store(), "", 99, 98);
  CHECK(run2.guestId() == first);
  CHECK(disk.saves == 1);
  CHECK(run2.lobbyUrl().empty());

  // A trailing newline (a hand-edited file) is fine; garbage is replaced.
  disk.text = first + "\n";
  LocalPlatformServices run3(disk.store(), "", 5, 6);
  CHECK(run3.guestId() == first);
  disk.text = "not an id";
  LocalPlatformServices run4(disk.store(), "", 5, 6);
  const std::string fresh = run4.guestId();
  CHECK(isGuestId(fresh));
  CHECK(fresh != first);
  CHECK(disk.text == fresh);
  CHECK(disk.saves == 2);
}

TEST_CASE("without storage the guest id lives for the run only", "[platform][services]") {
  LocalPlatformServices services(GuestIdStore{}, "", 7, 8);
  const std::string id = services.guestId();
  CHECK(isGuestId(id));
  CHECK(services.guestId() == id);
}

TEST_CASE("clipboard and share: null has neither, local goes through its hooks (v0.43)",
          "[platform][services][share]") {
  NullPlatformServices null;
  CHECK_FALSE(null.copyText("https://kfm.example/?room=ABCDE"));
  CHECK_FALSE(null.canShare());
  CHECK_FALSE(null.share("Kung Fu Monkeys", "Join my room", "https://kfm.example/?room=ABCDE"));

  // No hooks: a platform without a clipboard or a share sheet.
  LocalPlatformServices bare({}, {}, 1, 2);
  CHECK_FALSE(bare.copyText("x"));
  CHECK_FALSE(bare.canShare());
  CHECK_FALSE(bare.share("t", "x", "u"));

  // Recording fakes: what the app's SDL / navigator hooks would receive.
  std::string copied;
  std::string shared;
  bool touch = false;
  kfm::platform::ShareHooks hooks;
  hooks.copy = [&](std::string_view t) {
    copied = t;
    return true;
  };
  hooks.canShare = [&] { return touch; };
  hooks.share = [&](std::string_view title, std::string_view text, std::string_view url) {
    shared = std::string(title) + "|" + std::string(text) + "|" + std::string(url);
    return true;
  };
  LocalPlatformServices local({}, {}, 1, 2, hooks);
  CHECK(local.copyText("https://kfm.example/?room=ABCDE"));
  CHECK(copied == "https://kfm.example/?room=ABCDE");
  // A desktop browser (no touch): no share sheet, nothing shared.
  CHECK_FALSE(local.canShare());
  CHECK_FALSE(local.share("Kung Fu Monkeys", "Join my room", "u"));
  CHECK(shared.empty());
  touch = true;
  CHECK(local.canShare());
  CHECK(local.share("Kung Fu Monkeys", "Join my room", "https://kfm.example/?room=ABCDE"));
  CHECK(shared == "Kung Fu Monkeys|Join my room|https://kfm.example/?room=ABCDE");
}
