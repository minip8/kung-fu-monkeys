# Determinism gate (architecture §5.1, §9): every replay in the corpus must
# reproduce its recorded per-tick checksums on every OS, compiler, build
# type and in WebAssembly. The expected files are checked in, so the same
# numbers are compared everywhere; that is what makes this a cross-platform
# gate rather than a self-consistency check.
set(KFM_REPLAY_CORPUS_DIR "${PROJECT_SOURCE_DIR}/tools/replay-corpus")
file(GLOB KFM_CORPUS_REPLAYS CONFIGURE_DEPENDS "${KFM_REPLAY_CORPUS_DIR}/*.kfmr")

if(NOT KFM_CORPUS_REPLAYS)
  # Fail loudly at test time rather than configure time so a fresh checkout
  # can still build kfm_replay to create the first corpus entry.
  message(WARNING "KFM: replay corpus is empty (${KFM_REPLAY_CORPUS_DIR})")
  add_test(NAME determinism_corpus_missing COMMAND ${CMAKE_COMMAND} -E false)
endif()

foreach(replay IN LISTS KFM_CORPUS_REPLAYS)
  get_filename_component(name "${replay}" NAME_WE)
  set(expected "${KFM_REPLAY_CORPUS_DIR}/${name}.checksums")
  if(NOT EXISTS "${expected}")
    # Fails at test time; `cmake --build --target regen-replay-corpus` creates it.
    message(WARNING "KFM: missing ${expected} for ${replay}")
    add_test(NAME determinism_${name} COMMAND ${CMAKE_COMMAND} -E false)
    continue()
  endif()
  add_test(NAME determinism_${name}
    COMMAND kfm_replay verify "${replay}" "${expected}")
  set_tests_properties(determinism_${name} PROPERTIES LABELS determinism)
endforeach()

# Re-records every replay listed in tools/replay-corpus/corpus.txt and
# regenerates its expected checksum file from the current build. Run only
# after an intentional gameplay or content change, and say so in the commit
# message.
add_custom_target(regen-replay-corpus
  COMMAND ${CMAKE_COMMAND}
    -DKFM_REPLAY_EXE=$<TARGET_FILE:kfm_replay>
    "-DKFM_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
    -DKFM_CORPUS_DIR=${KFM_REPLAY_CORPUS_DIR}
    -P ${PROJECT_SOURCE_DIR}/tools/ci/RegenCorpus.cmake
  DEPENDS kfm_replay
  COMMENT "Re-recording the replay corpus and regenerating checksums"
  VERBATIM)
