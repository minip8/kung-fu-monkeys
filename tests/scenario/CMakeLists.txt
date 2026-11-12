# End-to-end scenario tests (architecture §9): scripted inputs -> expected
# events and state, one file per feature.
kfm_add_test_exe(kfm_scenario_tests
  dash_scenario.cpp
  throw_scenario.cpp
  catch_scenario.cpp
  hit_scenario.cpp
  round_scenario.cpp
  match_scenario.cpp
  scripted_match_scenario.cpp
  pipeline_scenario.cpp
  banana_supply_scenario.cpp
  dash_attack_scenario.cpp
  match_config_scenario.cpp
  arena_scenario.cpp
  banana_conservation_scenario.cpp
  banana_economy_scenario.cpp
  pit_scenario.cpp
  dry_round_scenario.cpp
  mode_scenario.cpp
  mode_hooks_scenario.cpp
  powerup_scenario.cpp
  blink_scenario.cpp
  slam_scenario.cpp
  parry_scenario.cpp
  call_scenario.cpp
  ability_bots_scenario.cpp
  team_scenario.cpp
  objective_scenario.cpp
  control_scenario.cpp
  tree_scenario.cpp
  harvest_scenario.cpp
  banana_flight_scenario.cpp
  grid_scenario.cpp
  side_symmetry_scenario.cpp
)
# side_symmetry_scenario.cpp plays the shipped contest row (the embedded
# content) and steers with the sim's own angleOf.
target_link_libraries(kfm_scenario_tests PRIVATE kfm::sim kfm::content kfm::sim_internal)
