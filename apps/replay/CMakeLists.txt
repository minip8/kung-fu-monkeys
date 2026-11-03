add_executable(kfm_replay main.cpp stats.cpp)
target_link_libraries(kfm_replay PRIVATE kfm::sim kfm::content kfm::warnings)
set_target_properties(kfm_replay PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # Node-only CLI: read host files directly, exit with the process code.
  target_link_options(kfm_replay PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
endif()
