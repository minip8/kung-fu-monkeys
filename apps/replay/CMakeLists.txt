add_executable(kfm_replay main.cpp stats.cpp)
target_link_libraries(kfm_replay PRIVATE kfm::sim kfm::content kfm::warnings)
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
endif()
