# Playable client (architecture §3, §11 M2): SDL3 window, sokol_gfx, Dear ImGui.
add_executable(kfm_client main.cpp)
target_link_libraries(kfm_client PRIVATE
  kfm::sim kfm::presentation kfm::platform_input kfm::platform kfm::warnings)
set_target_properties(kfm_client PROPERTIES FOLDER apps)

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

# Rendering smoke test without a human: 30 frames on a virtual X server with
# Mesa's software rasteriser. Linux only, and only when xvfb-run exists at
# configure time (CI installs xvfb and libgl1-mesa-dri).
if(KFM_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
  find_program(KFM_XVFB_RUN xvfb-run)
  if(KFM_XVFB_RUN)
    add_test(NAME client_smoke
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_client> --exit-after-frames 30)
    # Force X11 + llvmpipe so the test never depends on the host GPU or a
    # Wayland compositor; LSan reports leaks inside Mesa/X11, not ours.
    set_tests_properties(client_smoke PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
