// Which menu item is drawn highlighted (v0.55): the highlight follows the
// device last used for the menus. SDL- and ImGui-free; main.cpp feeds it
// events, the screens (through ui_style) only read it.
//
//   Pointer  the state at startup, and after any mouse motion or click: an
//            item is highlighted only while the cursor is over it (a touch
//            only while the finger is down), so a resting selection the
//            mouse user never pointed at is not drawn. The hovered item
//            takes the models' focus, so a later key press starts there.
//   Keys     after a menu navigation or select press from the keyboard or a
//            pad: the models' focused item is highlighted, the pointer's
//            hover is not.
//
// The first such press in Pointer mode only reveals the focus (reveal()
// returns true and the caller drops the press): an arrow does not move a
// selection the player could not see, and Enter / Space / a pad's South
// never activates one. With the pointer over the focused item (the hovered
// item took the focus) the press acts at once: Enter on a hovered button
// activates it.
#pragma once

#include "kfm/platform/input_event.hpp"

#include <cstdint>

namespace kfm::client {

class MenuHighlight {
 public:
  enum class Mode : uint8_t { Pointer = 0, Keys };

  // Mouse motion and mouse buttons switch to Pointer; other events leave
  // the mode alone (a key press counts only through reveal()).
  void onEvent(const platform::InputEvent& event);
  // A menu navigation or select press from the keyboard or a pad: switches
  // to Keys. True when it only revealed the focus, so the caller drops it:
  // it came in Pointer mode while the focused item was not under the pointer
  // (`pointerOnFocus`: the focus was visible already, so the press acts, and
  // Enter on a hovered button activates it).
  [[nodiscard]] bool reveal(bool pointerOnFocus);

  [[nodiscard]] Mode mode() const { return mode_; }
  [[nodiscard]] bool keys() const { return mode_ == Mode::Keys; }
  void setMode(Mode mode) { mode_ = mode; }

  // Whether an item is drawn highlighted: the focused one in Keys mode, the
  // one under the pointer in Pointer mode.
  [[nodiscard]] bool shows(bool focused, bool pointerOver) const {
    return keys() ? focused : pointerOver;
  }
  // Whether the pointer is over an item for shows(): a touch screen keeps
  // the last tap's position after the finger lifts, so a touch counts only
  // while pressed.
  [[nodiscard]] static bool pointerOver(bool hovered, bool touch, bool pressed) {
    return hovered && (!touch || pressed);
  }

 private:
  Mode mode_{Mode::Pointer};
};

}  // namespace kfm::client
