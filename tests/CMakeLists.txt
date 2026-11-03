# Test tree (architecture §9). Catch2 unit tests per module, scenario tests,
# and the determinism gate over the replay corpus.

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

add_subdirectory(unit)
add_subdirectory(scenario)
add_subdirectory(determinism)
