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
)
target_link_libraries(kfm_scenario_tests PRIVATE kfm::sim)
