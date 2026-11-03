# Playable client (architecture §3, §11 M2): SDL3 window, sokol_gfx, Dear ImGui.
#
#   kfm_client_core  headless match loop (MatchSession, fixed-step clock, lobby,
#                    bots, content tuning, options) and network mode
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
)
add_library(kfm::client_core ALIAS kfm_client_core)
target_include_directories(kfm_client_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_client_core
  PUBLIC kfm::sim kfm::net kfm::presentation kfm::platform_input
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_client_core)
set_target_properties(kfm_client_core PROPERTIES FOLDER apps)

add_library(kfm_client_sdl STATIC sdl_input.cpp)
add_library(kfm::client_sdl ALIAS kfm_client_sdl)
target_include_directories(kfm_client_sdl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_client_sdl
  PUBLIC kfm::platform_input SDL3::SDL3
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_client_sdl)
set_target_properties(kfm_client_sdl PROPERTIES FOLDER apps)

add_executable(kfm_client main.cpp hud.cpp debug_overlay.cpp)
target_link_libraries(kfm_client PRIVATE
  kfm::client_core kfm::client_sdl kfm::content kfm::renderer kfm::platform kfm::warnings)
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
              $<TARGET_FILE:kfm_client> --autostart --bots 3 --exit-after-frames 60)
    # Force X11 + llvmpipe so the test never depends on the host GPU or a
    # Wayland compositor; LSan reports leaks inside Mesa/X11, not ours.
    set_tests_properties(client_smoke PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
    # Network mode without a server: the connecting/disconnected screen
    # renders and the client exits 0 (nothing listens on port 1).
    add_test(NAME client_smoke_net
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client> --connect ws://127.0.0.1:1 --exit-after-frames 60)
    set_tests_properties(client_smoke_net PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
