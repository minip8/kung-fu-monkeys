# kfm_arena (architecture §8, §9): the headless match runner.
#
# kfm_arena_core  MatchSetup, runMatch, the result line and the summary:
#                 AgentHost over sim::step with bots built from BotSpecs.
#                 Headless, so it is unit-tested on every target
#                 (tests/unit/arena). Bots come from kfm::ai_bots's
#                 BotFactory; with KFM_WASM_BOTS it also links kfm::ai_wasm
#                 for the modules' stats; without (and under Emscripten,
#                 where the option is forced off) a module spec is refused.
# kfm_arena       the CLI. Everywhere, the server-headless preset included;
#                 under Emscripten a node CLI like kfm_replay (reference bots
#                 only).
#
# Both are built without exceptions or RTTI like the other apps.
add_library(kfm_arena_core STATIC arena_core.cpp)
add_library(kfm::arena_core ALIAS kfm_arena_core)
target_include_directories(kfm_arena_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_arena_core PUBLIC kfm::ai kfm::ai_bots kfm::sim
  PRIVATE kfm::build_id kfm::warnings)
if(KFM_WASM_BOTS)
  target_link_libraries(kfm_arena_core PRIVATE kfm::ai_wasm)
  target_compile_definitions(kfm_arena_core PRIVATE KFM_WASM_BOTS=1)
endif()
kfm_no_exceptions_rtti(kfm_arena_core)
set_target_properties(kfm_arena_core PROPERTIES FOLDER apps)

add_executable(kfm_arena main.cpp)
target_link_libraries(kfm_arena PRIVATE kfm::arena_core kfm::ai kfm::content kfm::sim kfm::warnings)
kfm_no_exceptions_rtti(kfm_arena)
set_target_properties(kfm_arena PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # Node-only CLI: read host files directly, exit with the process code.
  target_link_options(kfm_arena PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
endif()

# Smoke runs: four hard reference bots for two minutes, and an echo module
# against reference bots when modules are built. Exit 0 is the check.
if(KFM_BUILD_TESTS)
  add_test(NAME arena_smoke COMMAND kfm_arena --bots 4 --ticks 7200 --quiet)
  # v0.35: a 2v2 squad on the grid under fog, entrants rotated, bots wiped on respawn.
  add_test(NAME arena_smoke_team
    COMMAND kfm_arena --mode grid_rounds_1t --team ref:hard,ref:medium --team ref:hard
            --matches 2 --rotate --memory-on-respawn wipe --ticks 3600 --quiet)
  if(KFM_WASM_BOTS)
    add_test(NAME arena_smoke_wasm
      COMMAND kfm_arena --bot ${PROJECT_SOURCE_DIR}/tests/fixtures/bots/echo.wasm --bots 4
              --ticks 7200 --quiet)
  endif()
endif()
