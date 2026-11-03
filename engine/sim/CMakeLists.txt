# engine/sim: the deterministic simulation. Depends only on kfm::core
# (architecture §4). No exceptions, no RTTI, no floating point (CI greps).
add_library(kfm_sim STATIC
  src/content.cpp
  src/world_state.cpp
  src/step.cpp
  src/replay.cpp
  src/scripted_inputs.cpp
  src/abilities/registry.cpp
  src/abilities/dash.cpp
  src/abilities/throw.cpp
  src/rules/table.cpp
  src/rules/catch.cpp
  src/rules/damage.cpp
  src/rules/dash_attack.cpp
  src/rules/auto_catch.cpp
  src/rules/pickup.cpp
  src/rules/powerup.cpp
  src/rules/shield.cpp
  src/systems/rules.cpp
  src/systems/input.cpp
  src/systems/ability.cpp
  src/systems/movement.cpp
  src/systems/projectile.cpp
  src/systems/collision.cpp
  src/systems/interaction.cpp
  src/systems/modifier.cpp
  src/systems/lifecycle.cpp
)
add_library(kfm::sim ALIAS kfm_sim)

target_include_directories(kfm_sim
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_sim
  PUBLIC kfm::core
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_sim)

set_target_properties(kfm_sim PROPERTIES FOLDER engine)

# Internal headers under src/ (collision internals, angleOf) for white-box
# unit tests only: `#include "systems/collision_internal.hpp"`. Never link
# this from apps or other engine modules.
add_library(kfm_sim_internal INTERFACE)
add_library(kfm::sim_internal ALIAS kfm_sim_internal)
target_include_directories(kfm_sim_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_sim_internal INTERFACE kfm::sim)
