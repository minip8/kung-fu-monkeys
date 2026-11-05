#include "online_menu.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace kfm::client {

namespace {
constexpr std::array<OnlineRow, 5> kOnlineRows{OnlineRow::Region, OnlineRow::CreateRoom,
                                               OnlineRow::Code, OnlineRow::Join, OnlineRow::Back};
constexpr std::size_t kRoomCodeLength = 5;
}  // namespace

OnlineMenu::OnlineMenu() {
  for (const char* r : kRegions)
    regions_.push_back(RegionChoice{.name = r, .free = true});
}

void OnlineMenu::setRegions(std::span<const RegionChoice> regions) {
  if (regions.empty()) return;
  regions_.assign(regions.begin(), regions.end());
  if (std::ranges::find(regions_, region_, &RegionChoice::name) != regions_.end()) return;
  const auto free = std::ranges::find(regions_, true, &RegionChoice::free);
  region_ = (free != regions_.end() ? *free : regions_.front()).name;
}

std::span<const OnlineRow> OnlineMenu::rows() {
  return kOnlineRows;
}

void OnlineMenu::setFocus(std::size_t row) {
  if (row < kOnlineRows.size()) focus_ = row;
}

bool OnlineMenu::apply(MenuAction action) {
  const std::size_t n = kOnlineRows.size();
  switch (action) {
    case MenuAction::Up:
      focus_ = (focus_ + n - 1) % n;
      return false;
    case MenuAction::Down:
      focus_ = (focus_ + 1) % n;
      return false;
    case MenuAction::Left:
      return change(focus_, -1);
    case MenuAction::Right:
      return change(focus_, +1);
    case MenuAction::None:
      break;
  }
  return false;
}

bool OnlineMenu::change(std::size_t row, int direction) {
  if (row >= kOnlineRows.size() || direction == 0) return false;
  const OnlineRow r = kOnlineRows[row];
  if (r != OnlineRow::Region && r != OnlineRow::CreateRoom) return false;
  // By name: a region outside the list (--region xx) steps to the first.
  const std::string before = region_;
  std::size_t i = regions_.size();
  for (std::size_t k = 0; k < regions_.size(); ++k) {
    if (regions_[k].name == region_) i = k;
  }
  if (i == regions_.size()) {
    i = 0;
  } else if (direction > 0) {
    i = (i + 1) % regions_.size();
  } else {
    i = (i + regions_.size() - 1) % regions_.size();
  }
  region_ = regions_[i].name;
  return region_ != before;
}

void OnlineMenu::setCode(std::string_view text) {
  code_.clear();
  for (const char c : text) {
    if (code_.size() == kRoomCodeLength) break;
    if (!isRoomCodeChar(c)) continue;
    code_.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
}

bool OnlineMenu::codeValid() const {
  return normalizeRoomCode(code_).has_value();
}

OnlineMenu::Activation OnlineMenu::activate(std::size_t row) {
  if (row >= kOnlineRows.size()) return Activation::None;
  focus_ = row;
  switch (kOnlineRows[row]) {
    case OnlineRow::Region:
      return Activation::None;
    case OnlineRow::CreateRoom:
      command_ =
          LobbyCommand{.kind = LobbyCommand::Kind::CreateRoom, .region = region_, .code = {}};
      return Activation::Command;
    case OnlineRow::Code:
      return Activation::EditCode;
    case OnlineRow::Join:
      if (!codeValid()) return Activation::EditCode;
      command_ = LobbyCommand{.kind = LobbyCommand::Kind::JoinRoom, .region = {}, .code = code_};
      return Activation::Command;
    case OnlineRow::Back:
      return Activation::Back;
  }
  return Activation::None;
}

std::optional<LobbyCommand> OnlineMenu::takeCommand() {
  std::optional<LobbyCommand> c = std::move(command_);
  command_.reset();
  return c;
}

const char* OnlineMenu::rowLabel(OnlineRow row) {
  switch (row) {
    case OnlineRow::Region:
      return "Region";
    case OnlineRow::CreateRoom:
      return "Create room";
    case OnlineRow::Code:
      return "Room code";
    case OnlineRow::Join:
      return "Join room";
    case OnlineRow::Back:
      return "Back";
  }
  return "";
}

std::string OnlineMenu::regionText() const {
  const auto it = std::ranges::find(regions_, region_, &RegionChoice::name);
  return it != regions_.end() && !it->free ? region_ + " (no server)" : region_;
}

}  // namespace kfm::client
