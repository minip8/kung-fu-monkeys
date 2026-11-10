# kfm_replay_core: the contact-overflow accounting bench, verify and stats
# share (contact_drops.hpp), a library so tests/unit/replay can test it.
add_library(kfm_replay_core STATIC contact_drops.cpp)
add_library(kfm::replay_core ALIAS kfm_replay_core)
target_include_directories(kfm_replay_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_replay_core PUBLIC kfm::sim PRIVATE kfm::warnings)
set_target_properties(kfm_replay_core PROPERTIES FOLDER apps)

add_executable(kfm_replay main.cpp stats.cpp)
target_link_libraries(kfm_replay PRIVATE kfm::replay_core kfm::build_id kfm::sim kfm::content
  kfm::warnings)
set_target_properties(kfm_replay PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # Node-only CLI: read host files directly, exit with the process code.
  target_link_options(kfm_replay PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
endif()

# statsStep() (stats.cpp) re-implements step() so the feel stats can watch
# each contact; StatsRun (main.cpp) steps a shadow world with step() and the
# run fails (exit 1, "statsStep diverged") on the first checksum that
# differs. These runs exercise that guard over a free-for-all, power-ups with
# slam in a respawning mode, pits, and the grid squad with calls.
if(KFM_BUILD_TESTS)
  add_test(NAME replay_stats_smoke
    COMMAND kfm_replay stats --players 4 --matches 2 --ticks 3600 --quiet)
  add_test(NAME replay_stats_smoke_powerups
    COMMAND kfm_replay stats --players 6 --matches 1 --arena dojo_powerups --mode deathmatch
            --loadout slam --ticks 3600 --quiet)
  add_test(NAME replay_stats_smoke_pits
    COMMAND kfm_replay stats --players 4 --matches 1 --arena island --ticks 3600 --quiet)
  add_test(NAME replay_stats_smoke_grid
    COMMAND kfm_replay stats --players 4 --matches 1 --mode contest --ticks 3600 --quiet)
  set_tests_properties(replay_stats_smoke replay_stats_smoke_powerups replay_stats_smoke_pits
    replay_stats_smoke_grid PROPERTIES FAIL_REGULAR_EXPRESSION "diverged")

  # The contact-drop report of bench, verify and stats (issue #70): a match on
  # tests/fixtures/content/crowd.toml (added to a copy of content/) overflows
  # the contact list, so the line and verify --fail-on-dropped-contacts are
  # checked end to end; the shipped content must print no such line.
  add_test(NAME replay_contact_drops
    COMMAND ${CMAKE_COMMAND}
      -DKFM_REPLAY_EXE=$<TARGET_FILE:kfm_replay>
      "-DKFM_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
      -DKFM_CONTENT_DIR=${KFM_CONTENT_SOURCE_DIR}
      -DKFM_CROWD_TOML=${PROJECT_SOURCE_DIR}/tests/fixtures/content/crowd.toml
      -DKFM_CORPUS_REPLAY=${PROJECT_SOURCE_DIR}/tools/replay-corpus/scripted_10p_3600t.kfmr
      -DKFM_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/contact_drops
      -P ${CMAKE_CURRENT_SOURCE_DIR}/contact_drops_test.cmake)
endif()
