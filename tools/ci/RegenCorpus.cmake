# cmake -DKFM_REPLAY_EXE=<path> [-DKFM_EMULATOR=<node>] -DKFM_CORPUS_DIR=<dir> -P RegenCorpus.cmake
# Re-records every replay listed in <dir>/corpus.txt with the current build
# (so each carries this build's content hash: the embedded content/*.toml),
# then rewrites <name>.checksums
# next to it. A .kfmr in the corpus without a manifest line is an error: it
# could not be re-recorded after a content change.
set(manifest "${KFM_CORPUS_DIR}/corpus.txt")
if(NOT EXISTS "${manifest}")
  message(FATAL_ERROR "missing replay corpus manifest ${manifest}")
endif()
file(STRINGS "${manifest}" lines ENCODING UTF-8)
set(names "")
foreach(line IN LISTS lines)
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  string(REGEX REPLACE "[ \t]+" ";" fields "${line}")
  list(LENGTH fields n)
  if(NOT n EQUAL 6 AND NOT n EQUAL 8 AND NOT n EQUAL 9)
    message(FATAL_ERROR "corpus.txt: expected 6, 8 or 9 fields, got ${n}: '${line}'")
  endif()
  list(GET fields 0 name)
  list(GET fields 1 kind)
  list(GET fields 2 seed)
  list(GET fields 3 inputSeed)
  list(GET fields 4 players)
  list(GET fields 5 ticks)
  set(replay "${KFM_CORPUS_DIR}/${name}.kfmr")
  set(args --seed ${seed} --players ${players} --ticks ${ticks})
  if(n GREATER_EQUAL 8)
    # Optional arena and mode names (MatchConfig::arena / ::mode); without
    # them the replay uses the first of each, the default match.
    list(GET fields 6 arena)
    list(GET fields 7 mode)
    list(APPEND args --arena ${arena} --mode ${mode})
  endif()
  if(n EQUAL 9)
    # Optional power-up set (MatchConfig::powerUps): all, none or
    # comma-separated names; without it every power-up is enabled.
    list(GET fields 8 powerUps)
    list(APPEND args --powerups ${powerUps})
  endif()
  if(kind STREQUAL "empty")
    set(record record-empty)
  elseif(kind STREQUAL "scripted")
    set(record record-scripted)
    list(APPEND args --input-seed ${inputSeed})
  else()
    message(FATAL_ERROR "corpus.txt: unknown kind '${kind}' for ${name}")
  endif()
  execute_process(COMMAND ${KFM_EMULATOR} "${KFM_REPLAY_EXE}" ${record} "${replay}" ${args}
    OUTPUT_QUIET RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kfm_replay ${record} failed for ${name} (rc=${rc})")
  endif()
  execute_process(COMMAND ${KFM_EMULATOR} "${KFM_REPLAY_EXE}" checksums "${replay}"
    OUTPUT_FILE "${KFM_CORPUS_DIR}/${name}.checksums"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "kfm_replay checksums failed for ${name} (rc=${rc})")
  endif()
  list(APPEND names "${name}")
  message(STATUS "re-recorded ${name}.kfmr and regenerated ${name}.checksums")
endforeach()

file(GLOB replays "${KFM_CORPUS_DIR}/*.kfmr")
foreach(replay IN LISTS replays)
  get_filename_component(name "${replay}" NAME_WE)
  if(NOT name IN_LIST names)
    message(FATAL_ERROR "${replay} has no line in ${manifest}; add one so it can be re-recorded")
  endif()
endforeach()
