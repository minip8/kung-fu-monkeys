# engine/ai: the agent platform (architecture §8). IAgent, the versioned
# Observation and its builder, the built-in reference bot and WasmAgent over
# the metered wasm runtime. Depends on kfm::sim and kfm::core only, never on
# net, presentation or platform (§4). Agents live outside the sim: nothing
# here writes WorldState. No exceptions, no RTTI like the other engine
# modules; the wasm runtime wrapper catches nothing because WAMR is C.
add_library(kfm_ai STATIC
  src/agent.cpp
  src/bot_spec.cpp
  src/agent_host.cpp
  src/observation_builder.cpp
  src/reference_bot.cpp
  src/vision.cpp
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

# ---------------------------------------------------------------------------
# kfm_ai_wasm (kfm::ai_wasm): WasmRuntime, WasmModule and WasmAgent, the
# IAgent over a sandboxed, instruction-metered bot module (§8), on WAMR's
# classic interpreter (kfm::wamr, third_party/wamr). Native only and only with
# KFM_WASM_BOTS: the browser runs bot modules through its own WebAssembly API.
# A separate target so kfm_ai and everything linking it never see WAMR; its
# public header includes nothing from it. WAMR is C and reports errors as
# strings, so this one is exception- and RTTI-free like the rest of the
# engine.
# ---------------------------------------------------------------------------
if(KFM_WASM_BOTS)
  add_library(kfm_ai_wasm STATIC src/wasm_runtime.cpp)
  add_library(kfm::ai_wasm ALIAS kfm_ai_wasm)
  target_include_directories(kfm_ai_wasm
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(kfm_ai_wasm
    PUBLIC kfm::ai
    PRIVATE kfm::wamr kfm::warnings)
  kfm_no_exceptions_rtti(kfm_ai_wasm)
  set_target_properties(kfm_ai_wasm PROPERTIES FOLDER engine)
endif()
