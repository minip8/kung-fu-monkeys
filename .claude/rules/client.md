---
paths:
  - "apps/client/**"
  - "tests/unit/client/**"
  - "assets/fonts/**"
---

# Client rules

These add to the platform and presentation rules in `CLAUDE.md`, which always apply.

- The UI is Dear ImGui with the game font and the palette of `apps/client/ui_style.hpp` (no
  colour literals in the HUD or screens); screen state lives in headless models in
  `apps/client/core` (`menus.hpp`, `online_menu.hpp`, `scoreboard.hpp`, `hud_view.hpp`), tested
  without a window; hud.cpp / screens.cpp only draw them. `assets/fonts/` holds source assets
  with their licence and is compiled in (never read at run time: the browser has no
  filesystem).
- Clipboard, sharing, saved preferences, the page URL and the replay download go through
  `IPlatformServices` (one `PlatformHooks` value built in
  `apps/client/client_platform_native.cpp` or `client_platform_web.cpp`), never SDL or JS
  directly: `main.cpp` has no `EM_JS`.
- What is highlighted follows the last menu device (`core/menu_highlight.hpp`): mouse mode
  highlights only the hovered item, keys/pad mode the focus, and the first key press from mouse
  mode only reveals it. Draw menu buttons and rows through `ui::bigButton` / `ui::showsFocus` /
  `ui::pointerTakesFocus`, never from `focus()` alone.
- A frame in which a new ImGui window measures itself is not presented
  (`debugUiMeasuring()`); a screen's panel card drawn outside its window must not be relied on
  in that frame.
- The between-rounds scoreboard shows in RoundEnded and at match end only, never over the next
  countdown; a round's pause is 180 ticks (`round_end_ticks`).
- The mouse aims from the local monkey (the predicted one online) through `feedMouseAim` in
  `apps/client/core/mouse_aim.hpp`; clicks over UI windows stay the UI's.
