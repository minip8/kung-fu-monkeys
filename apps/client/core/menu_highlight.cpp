#include "menu_highlight.hpp"

namespace kfm::client {

void MenuHighlight::onEvent(const platform::InputEvent& event) {
  using platform::InputEventKind;
  if (event.kind == InputEventKind::MouseMotion || event.kind == InputEventKind::MouseButtonDown) {
    mode_ = Mode::Pointer;
  }
}

bool MenuHighlight::reveal(bool pointerOnFocus) {
  if (mode_ == Mode::Keys) return false;
  mode_ = Mode::Keys;
  return !pointerOnFocus;
}

}  // namespace kfm::client
