# One test executable per module. Tests may use floating point (reference
# comparisons) and exceptions (Catch2); the libraries under test may not.
function(kfm_add_test_exe name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain kfm::warnings)
  set_target_properties(${name} PROPERTIES FOLDER tests)
  if(EMSCRIPTEN)
    target_link_options(${name} PRIVATE -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1)
  endif()
  # Plain add_test rather than catch_discover_tests: one process per module
  # is fast and works unchanged under the Emscripten node emulator.
  add_test(NAME ${name} COMMAND ${name})
endfunction()

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
)
target_link_libraries(kfm_sim_tests PRIVATE kfm::sim)
