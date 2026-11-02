# Always built as C++23 so it keeps answering the same question even if
# KFM_CXX_STANDARD is overridden.
add_executable(kfm_cxx23_probe probe.cpp)
target_link_libraries(kfm_cxx23_probe PRIVATE kfm::warnings)
set_target_properties(kfm_cxx23_probe PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  FOLDER tools)
if(EMSCRIPTEN)
  target_link_options(kfm_cxx23_probe PRIVATE -sEXIT_RUNTIME=1)
endif()
if(BUILD_TESTING)
  add_test(NAME cxx23_probe COMMAND kfm_cxx23_probe)
endif()
