# Render probe (architecture §7, §11 M2): a scripted match drawn by kfm_renderer,
# for checking the 3D presentation and taking screenshots without a human.
add_executable(kfm_render_probe main.cpp)
target_link_libraries(kfm_render_probe PRIVATE
  kfm::sim kfm::presentation kfm::renderer kfm::platform kfm::warnings)
set_target_properties(kfm_render_probe PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # Browser build: kfm_render_probe.html + .js + .wasm, reusing the client's
  # shell (canvas fills the viewport, WebGL2 context attributes per §6.8).
  # Never a ctest.
  set_target_properties(kfm_render_probe PROPERTIES SUFFIX ".html")
  set(kfm_render_probe_shell "${PROJECT_SOURCE_DIR}/apps/client/shell.html")
  target_link_options(kfm_render_probe PRIVATE
    --shell-file "${kfm_render_probe_shell}"
    -sSTACK_SIZE=1MB              # step() keeps ~40 KB of per-tick scratch on the stack
    -sALLOW_MEMORY_GROWTH=1
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sENVIRONMENT=web)
  set_property(TARGET kfm_render_probe APPEND PROPERTY LINK_DEPENDS "${kfm_render_probe_shell}")
endif()

# Rendering smoke test of the renderer on a scripted match: 120 frames at 4
# ticks per frame on a virtual X server with Mesa's software rasteriser. Linux
# only, and only when xvfb-run exists at configure time (mirrors client_smoke).
if(KFM_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT EMSCRIPTEN)
  find_program(KFM_XVFB_RUN xvfb-run)
  if(KFM_XVFB_RUN)
    add_test(NAME render_probe_smoke
      COMMAND ${KFM_XVFB_RUN} -a -s "-screen 0 1280x720x24"
              $<TARGET_FILE:kfm_render_probe> --players 6 --ticks-per-frame 4
              --exit-after-frames 120)
    set_tests_properties(render_probe_smoke PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=x11;LIBGL_ALWAYS_SOFTWARE=1;ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
