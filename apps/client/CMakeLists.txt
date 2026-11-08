# Playable client (architecture §3, §11 M2): SDL3 window, sokol_gfx, Dear ImGui.
#
#   kfm_client_core  headless match loop (MatchSession, fixed-step clock, lobby,
#                    the replay viewer's ReplaySession and ReplayControls,
#                    bots through kfm::ai's AgentHost, content tuning, options)
#                    and network mode
#                    (NetSession over any ITransport); no SDL, sokol, ImGui or
#                    WebRTC, so tests/unit/client exercises it without a window.
#   kfm_client_sdl   SDL_Event -> platform::InputEvent translation and gamepad
#                    lifetime (tested too: it is pure apart from SDL headers).
#   kfm_client       the executable: frame wiring, HUD, lobby, debug overlay.
add_library(kfm_client_core STATIC
  core/fixed_step_clock.cpp
  core/match_session.cpp
  core/content_tuning.cpp
  core/client_options.cpp
  core/net_session.cpp
  core/lobby_settings.cpp
  core/lobby_menu.cpp
  core/menus.cpp
  core/menu_highlight.cpp
  core/online_menu.cpp
  core/scoreboard.cpp
  core/bot_roster.cpp
  core/mouse_aim.cpp
  core/replay_session.cpp
  core/replay_controls.cpp
)
add_library(kfm::client_core ALIAS kfm_client_core)
target_include_directories(kfm_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_client_core
  PUBLIC kfm::sim kfm::ai kfm::net kfm::presentation kfm::platform_input
  PRIVATE kfm::content kfm::warnings)
kfm_no_exceptions_rtti(kfm_client_core)
# M6a: the lobby a build points at by default (--lobby, the browser page's
# config.js wins); empty: rooms need --lobby or ?lobby=.
set(KFM_LOBBY_URL "" CACHE STRING "Default lobby base URL of kfm_client (ws[s]://host[:port])")
target_compile_definitions(kfm_client_core PRIVATE "KFM_DEFAULT_LOBBY_URL=\"${KFM_LOBBY_URL}\"")
# v0.43: the web page a native build's invite links point at (Copy link
# copies <page>?room=CODE); empty: the native client copies the code alone.
set(KFM_PAGE_URL "" CACHE STRING "Web page URL of the client for native invite links (https://...)")
target_compile_definitions(kfm_client_core PRIVATE "KFM_DEFAULT_PAGE_URL=\"${KFM_PAGE_URL}\"")
# Bot modules as local players (§8) natively, over the same WAMR runtime as
# the server and kfm_arena. Without it (the browser) a module spec falls back
# to the reference bot. PUBLIC so the tests know which build they test.
if(KFM_WASM_BOTS)
  target_link_libraries(kfm_client_core PRIVATE kfm::ai_wasm)
  target_compile_definitions(kfm_client_core PUBLIC KFM_CLIENT_WASM_BOTS=1)
endif()
set_target_properties(kfm_client_core PROPERTIES FOLDER apps)

add_library(kfm_client_sdl STATIC sdl_input.cpp)
add_library(kfm::client_sdl ALIAS kfm_client_sdl)
target_include_directories(kfm_client_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_client_sdl
  PUBLIC kfm::platform_input SDL3::SDL3
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_client_sdl)
set_target_properties(kfm_client_sdl PROPERTIES FOLDER apps)

# v0.43: the UI font (assets/fonts/, OFL), compiled into the client like
# content/: the browser has no filesystem, and nothing is read at run time.
set(kfm_client_font "${PROJECT_SOURCE_DIR}/assets/fonts/LilitaOne-Regular.ttf")
set(kfm_client_font_cpp "${CMAKE_CURRENT_BINARY_DIR}/ui_font.cpp")
add_custom_command(
  OUTPUT "${kfm_client_font_cpp}"
  COMMAND ${CMAKE_COMMAND}
    "-DKFM_IN=${kfm_client_font}"
    "-DKFM_OUT=${kfm_client_font_cpp}"
    "-DKFM_NAMESPACE=kfm::client::assets"
    "-DKFM_SYMBOL=kUiFont"
    -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  DEPENDS "${kfm_client_font}" "${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake"
  COMMENT "Embedding the UI font"
  VERBATIM)

add_executable(kfm_client main.cpp hud.cpp screens.cpp ui_style.cpp debug_overlay.cpp
  client_audio.cpp "${kfm_client_font_cpp}")
target_link_libraries(kfm_client PRIVATE
  kfm::client_core kfm::client_sdl kfm::content kfm::renderer kfm::platform
  kfm::platform_services kfm::warnings)
kfm_no_exceptions_rtti(kfm_client)
set_target_properties(kfm_client PROPERTIES FOLDER apps)
# The rigged test model next to the executable (embedded under Emscripten).
kfm_use_test_rig(kfm_client)
# content/*.toml next to the executable (the browser build uses the embedded copy).
kfm_use_content(kfm_client)

if(EMSCRIPTEN)
  # Browser build: kfm_client.html + .js + .wasm from a custom shell (canvas
  # fills the viewport, WebGL2 context attributes per §6.8). Never a ctest.
  set_target_properties(kfm_client PROPERTIES SUFFIX ".html")
  set(kfm_client_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
  target_link_options(kfm_client PRIVATE
    --shell-file "${kfm_client_shell}"
    -sSTACK_SIZE=1MB              # step() keeps ~40 KB of per-tick scratch on the stack
    -sALLOW_MEMORY_GROWTH=1
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sENVIRONMENT=web)
  set_property(TARGET kfm_client APPEND PROPERTY LINK_DEPENDS "${kfm_client_shell}")
  # The page loads config.js (M6a): a default one beside the build output so
  # tools/web/serve.py serves no 404; the deploy workflow writes its own.
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/config.js"
    CONTENT "window.KFM_CONFIG = { lobbyUrl: \"${KFM_LOBBY_URL}\", sentryDsn: \"\" };\n")
  # The page's icon (shell.html's <link rel="icon">): the source SVG beside the
  # page; the deploy workflow copies it into the site. configure_file re-runs
  # the configure step when the SVG changes.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/favicon.svg"
    "${CMAKE_CURRENT_BINARY_DIR}/favicon.svg" COPYONLY)
endif()

# Network mode (--connect) runs NetSession over WebRtcTransport, natively and
# in the browser (datachannel-wasm: kfm::net_webrtc brings -fwasm-exceptions,
# its --js-library files and the EXPORTED_RUNTIME_METHODS it needs). Without
# KFM_WEBRTC the client builds and --connect reports that it cannot connect.
if(KFM_WEBRTC)
  target_link_libraries(kfm_client PRIVATE kfm::net_webrtc)
  target_compile_definitions(kfm_client PRIVATE KFM_CLIENT_WEBRTC=1)
endif()

# Rendering smoke test without a human: 60 frames of a started match with
# three bots on a virtual X server with Mesa's software rasteriser; a renderer
# that fails to initialise exits non-zero. Linux only, and only when xvfb-run exists at
# configure time (CI installs xvfb and libgl1-mesa-dri).
if(KFM_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
  find_program(KFM_XVFB_RUN xvfb-run)
  if(KFM_XVFB_RUN)
    add_test(NAME client_smoke
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client> --autostart --bots 3 --exit-after-frames 60 --no-settings)
    # Force X11 + llvmpipe so the test never depends on the host GPU or a
    # Wayland compositor; LSan reports leaks inside Mesa/X11, not ours.
    set_tests_properties(client_smoke PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
    # Network mode without a server: the connecting/disconnected screen
    # renders and the client exits 0 (nothing listens on port 1).
    add_test(NAME client_smoke_net
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client> --connect ws://127.0.0.1:1 --exit-after-frames 60
              --no-settings)
    set_tests_properties(client_smoke_net PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
    # M7 step 6: the replay viewer plays a corpus replay (recorded with the
    # content built into the client), and a file that is not a replay shows
    # the error screen and makes the run exit non-zero.
    add_test(NAME client_smoke_replay
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client>
              --replay "${PROJECT_SOURCE_DIR}/tools/replay-corpus/scripted_10p_contest_1800t.kfmr"
              --exit-after-frames 60 --no-settings)
    add_test(NAME client_smoke_replay_bad
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client>
              --replay "${PROJECT_SOURCE_DIR}/tools/replay-corpus/corpus.txt"
              --exit-after-frames 60 --no-settings)
    set_tests_properties(client_smoke_replay client_smoke_replay_bad PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
    set_tests_properties(client_smoke_replay_bad PROPERTIES WILL_FAIL TRUE)
  endif()
endif()
