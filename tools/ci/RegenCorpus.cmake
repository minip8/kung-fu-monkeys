# cmake -DKFM_REPLAY_EXE=<path> [-DKFM_EMULATOR=<node>] -DKFM_CORPUS_DIR=<dir> -P RegenCorpus.cmake
# Rewrites <name>.checksums next to every <name>.kfmr in the corpus.
file(GLOB replays "${KFM_CORPUS_DIR}/*.kfmr")
foreach(replay IN LISTS replays)
  get_filename_component(name "${replay}" NAME_WE)
  set(cmd ${KFM_EMULATOR} "${KFM_REPLAY_EXE}" checksums "${replay}")
  execute_process(COMMAND ${cmd}
    OUTPUT_FILE "${KFM_CORPUS_DIR}/${name}.checksums"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kfm_replay checksums failed for ${replay} (rc=${rc})")
  endif()
  message(STATUS "regenerated ${name}.checksums")
endforeach()
