# engine/presentation: reads sim state, never writes it (architecture §4, §7).
# kfm_presentation is the headless part (RenderState derivation, interpolation,
# smoothing, camera math, procedural meshes) and is unit-tested without a GPU.
# The sokol_gfx renderer is a separate target added below it and is skipped in
# KFM_HEADLESS builds.
add_library(kfm_presentation STATIC
  src/math.cpp
  src/camera.cpp
  src/builder.cpp
  src/meshes.cpp
)
add_library(kfm::presentation ALIAS kfm_presentation)

target_include_directories(kfm_presentation
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_presentation
  PUBLIC kfm::sim
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_presentation)

set_target_properties(kfm_presentation PROPERTIES FOLDER engine)
