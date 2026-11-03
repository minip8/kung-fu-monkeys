# engine/platform: the only engine module that touches the OS (architecture
# §4, §13). Two targets:
#   kfm_platform_input  SDL-free input sampling (InputEvent -> sim::PlayerInput,
#                       local-multiplayer bindings, sub-tick stamps); unit-tested
#                       headlessly.
#   kfm_platform        SDL3 window/GL context/events/gamepads/main loop plus the
#                       sokol_gfx and Dear ImGui glue; added by the platform work
#                       unit below.
add_library(kfm_platform_input STATIC
  src/input_sampler.cpp
)
add_library(kfm::platform_input ALIAS kfm_platform_input)
target_include_directories(kfm_platform_input
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_platform_input
  PUBLIC kfm::sim
  PRIVATE kfm::warnings)
set_target_properties(kfm_platform_input PROPERTIES FOLDER engine)


# ---------------------------------------------------------------------------
# kfm_platform (SDL3 + sokol_gfx + Dear ImGui) is defined below this line.
# ---------------------------------------------------------------------------
# Only engine/platform and the apps touch SDL; the renderer in
# engine/presentation gets sokol_gfx from kfm::sokol directly (cmake/
# KfmDependencies.cmake) and never links kfm_platform (§4).
add_library(kfm_platform STATIC
  src/window.cpp
  src/gfx.cpp
  src/debug_ui.cpp
  src/main_loop.cpp
)
add_library(kfm::platform ALIAS kfm_platform)
target_include_directories(kfm_platform
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SDL3 is PUBLIC because window.hpp exposes SDL_Event to the per-event
# callback (the input-event translation seam); Dear ImGui because apps build
# their debug windows with imgui.h directly.
target_link_libraries(kfm_platform
  PUBLIC SDL3::SDL3 kfm::imgui
  PRIVATE kfm::sokol kfm::warnings)
if(NOT EMSCRIPTEN)
  target_link_libraries(kfm_platform PRIVATE kfm::glad)
endif()
# SDL3, Dear ImGui and sokol need neither exceptions nor RTTI.
kfm_no_exceptions_rtti(kfm_platform)
set_target_properties(kfm_platform PROPERTIES FOLDER engine)
