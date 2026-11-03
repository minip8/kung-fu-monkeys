# One test executable per module (kfm_add_test_exe is defined in tests/).
kfm_add_test_exe(kfm_core_tests
  core/fixed_test.cpp
  core/vec2_test.cpp
  core/pool_test.cpp
  core/ring_buffer_test.cpp
  core/bitstream_test.cpp
  core/rng_test.cpp
  core/hash_test.cpp
)
target_link_libraries(kfm_core_tests PRIVATE kfm::core)

kfm_add_test_exe(kfm_sim_tests
  sim/visit_test.cpp
  sim/replay_test.cpp
  sim/step_test.cpp
  sim/content_test.cpp
  sim/tags_test.cpp
  sim/input_test.cpp
  sim/ability_test.cpp
  sim/movement_test.cpp
  sim/projectile_test.cpp
  sim/collision_test.cpp
  sim/interaction_test.cpp
  sim/modifier_test.cpp
  sim/lifecycle_test.cpp
  sim/rules_test.cpp
)
target_link_libraries(kfm_sim_tests PRIVATE kfm::sim kfm::sim_internal)

kfm_add_test_exe(kfm_presentation_tests
  presentation/render_state_test.cpp
)
target_link_libraries(kfm_presentation_tests PRIVATE kfm::presentation)

if(NOT KFM_HEADLESS)
  kfm_add_test_exe(kfm_platform_tests
    platform/input_sampler_test.cpp
  )
  target_link_libraries(kfm_platform_tests PRIVATE kfm::platform_input)
endif()
