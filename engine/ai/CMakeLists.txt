# engine/ai: the agent platform (architecture §8). IAgent, the versioned
# Observation and its builder, the built-in reference bot and WasmAgent over
# the metered wasm runtime. Depends on kfm::sim and kfm::core only, never on
# net, presentation or platform (§4). Agents live outside the sim: nothing
# here writes WorldState. No exceptions, no RTTI like the other engine
# modules; the wasm runtime wrapper catches nothing because WAMR is C.
add_library(kfm_ai STATIC
  src/agent.cpp
  src/agent_host.cpp
  src/observation_builder.cpp
  src/reference_bot.cpp
)
add_library(kfm::ai ALIAS kfm_ai)

target_include_directories(kfm_ai
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_ai
  PUBLIC kfm::sim
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_ai)

set_target_properties(kfm_ai PROPERTIES FOLDER engine)
