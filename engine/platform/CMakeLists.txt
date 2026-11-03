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
