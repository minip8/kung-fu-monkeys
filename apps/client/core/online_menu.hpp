// The Play online screen as a headless model (architecture §13.5, v0.43; the
// M6a Online section of the lobby panel moved here in M6b step 3): v0.94 the
// player's name first, then the region with its availability (lobby protocol
// v1.1), Create room, the room code field, Join and Back. SDL- and
// ImGui-free; screens.cpp draws it, main.cpp asks the lobby for its regions
// (setRegions()), saves the name and turns a LobbyCommand into a NetSession.
//
// Up/Down move the focus (wrapping), Left/Right cycle the region on the
// Region row (also from Create room, which names it). activate() on Create
// room or on Join with a valid code makes a command (takeCommand()); on the
// name, it asks the screen to put the keyboard into the name field
// (Activation::EditName); on the code field, or Join without a valid code,
// into the code field (Activation::EditCode); on Back it says so. v0.94: the
// screen opens on the name while the player has never chosen one (the
// random monkey name the client picked is offered and works as it is),
// otherwise on Create room (open()).
#pragma once

#include "client_options.hpp"
#include "lobby_menu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::client {

// What the Play online screen asks the app to do.
struct LobbyCommand {
  enum class Kind : uint8_t { CreateRoom = 0, JoinRoom };
  Kind kind{Kind::CreateRoom};
  std::string region;  // CreateRoom
  std::string code;    // JoinRoom: normalised upper case
  bool operator==(const LobbyCommand&) const = default;
};

// One region of the region row: a lobby region and whether the lobby last
// reported a free server in it ("eu" vs "us (no server)").
struct RegionChoice {
  std::string name;
  bool free{true};
  friend bool operator==(const RegionChoice&, const RegionChoice&) = default;
};

enum class OnlineRow : uint8_t { Name = 0, Region, CreateRoom, Code, Join, Back };

class OnlineMenu {
 public:
  enum class Activation : uint8_t { None = 0, Command, EditCode, Back, EditName };

  OnlineMenu();

  // v0.94: a visit to the screen starts here: the focus on the name when
  // `nameChosen` is false (a first visit), else on Create room.
  void open(bool nameChosen);

  // The regions the region row cycles through: kRegions (availability
  // unknown, shown as free) until the lobby answers a regions query, then
  // every region it listed with its availability. An empty list is ignored;
  // a current region outside the new list becomes its first free one, else
  // its first. A busy region stays selectable: the lobby refuses the create
  // no_server if it is still busy.
  void setRegions(std::span<const RegionChoice> regions);
  [[nodiscard]] std::span<const RegionChoice> regions() const { return regions_; }
  // The region (saved with the settings as `region`); any label is kept.
  [[nodiscard]] const std::string& region() const { return region_; }
  void setRegion(std::string region) { region_ = std::move(region); }

  [[nodiscard]] static std::span<const OnlineRow> rows();
  [[nodiscard]] std::size_t focus() const { return focus_; }
  [[nodiscard]] OnlineRow focusedRow() const { return rows()[focus_]; }
  void setFocus(std::size_t row);

  // Up/Down move the focus; Left/Right on the Region or Create room row
  // cycle the region. True when the region changed.
  bool apply(MenuAction action);
  bool change(std::size_t row, int direction);

  // The room code field: only the room alphabet, upper case, at most 5.
  [[nodiscard]] const std::string& code() const { return code_; }
  void setCode(std::string_view text);
  [[nodiscard]] bool codeValid() const;

  [[nodiscard]] Activation activate() { return activate(focus_); }
  [[nodiscard]] Activation activate(std::size_t row);
  // The pending command, once.
  [[nodiscard]] std::optional<LobbyCommand> takeCommand();

  [[nodiscard]] static const char* rowLabel(OnlineRow row);
  // v0.94: the line under the name field: why it matters, or (first visit)
  // that the offered name is fine to keep.
  [[nodiscard]] static const char* nameHint(bool nameChosen);
  // The region as shown: "eu", "us (no server)", a label outside the list bare.
  [[nodiscard]] std::string regionText() const;

 private:
  std::vector<RegionChoice> regions_;
  std::string region_{kRegions[0]};
  std::string code_;
  std::optional<LobbyCommand> command_;
  std::size_t focus_{static_cast<std::size_t>(OnlineRow::CreateRoom)};
};

}  // namespace kfm::client
