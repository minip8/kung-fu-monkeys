# engine/sim: the deterministic simulation. Depends only on kfm::core
# (architecture §4). No exceptions, no RTTI, no floating point (CI greps).
add_library(kfm_sim STATIC
  src/content.cpp
  src/world_state.cpp
  src/step.cpp
  src/replay.cpp
)
add_library(kfm::sim ALIAS kfm_sim)

target_include_directories(kfm_sim PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(kfm_sim
  PUBLIC kfm::core
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_sim)

set_target_properties(kfm_sim PROPERTIES FOLDER engine)
