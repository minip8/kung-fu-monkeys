# cmake -DKFM_REPLAY_EXE=<path> [-DKFM_EMULATOR=<node>] -DKFM_CONTENT_DIR=<content/>
#       -DKFM_CROWD_TOML=<tests/fixtures/content/crowd.toml>
#       -DKFM_CORPUS_REPLAY=<a corpus .kfmr, its .checksums beside it> -DKFM_WORK_DIR=<dir>
#       -P contact_drops_test.cmake
#
# kfm_replay's contact-drop report (issue #70) end to end. A copy of
# content/ plus crowd.toml (an arena and mode whose scripted match overflows
# the contact list) is recorded, then verify must print the warning line on
# stderr and exit 0, verify --fail-on-dropped-contacts the error line and
# exit 1, and bench and stats (--quiet) the warning naming the match. On the
# shipped content (a corpus replay, the default bench and stats) nothing is
# dropped and the output carries no such line: verify prints its OK line
# alone and exits 0 even with the flag.
foreach(var KFM_REPLAY_EXE KFM_CONTENT_DIR KFM_CROWD_TOML KFM_CORPUS_REPLAY KFM_WORK_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "contact_drops_test.cmake: ${var} is not set")
  endif()
endforeach()

set(content "${KFM_WORK_DIR}/content")
set(replay "${KFM_WORK_DIR}/crowd.kfmr")
set(sums "${KFM_WORK_DIR}/crowd.checksums")
file(REMOVE_RECURSE "${KFM_WORK_DIR}")
file(COPY "${KFM_CONTENT_DIR}/" DESTINATION "${content}")
# Sorted after every shipped file, so the shipped arenas and modes keep their indices.
file(COPY_FILE "${KFM_CROWD_TOML}" "${content}/zz-crowd.toml")
set(crowd --content "${content}")
set(match ${crowd} --arena crowd --mode crowd --players 10 --ticks 600)

# run(<name> <expected rc> <args>...): runs kfm_replay, sets <name>_out and
# <name>_err, fails unless the exit status is the expected one.
function(run name rc)
  execute_process(COMMAND ${KFM_EMULATOR} "${KFM_REPLAY_EXE}" ${ARGN}
    RESULT_VARIABLE got OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT got STREQUAL "${rc}")
    message(FATAL_ERROR "kfm_replay ${ARGN}: exit ${got}, expected ${rc}\n"
                        "stdout:\n${out}\nstderr:\n${err}")
  endif()
  set(${name}_out "${out}" PARENT_SCOPE)
  set(${name}_err "${err}" PARENT_SCOPE)
endfunction()

function(expect_match what text regex)
  if(NOT text MATCHES "${regex}")
    message(FATAL_ERROR "${what}: expected to match '${regex}', got:\n${text}")
  endif()
endfunction()

set(n "[0-9]+")
set(line "contact list overflow \\(kMaxContacts ${n}\\): ${n} contacts dropped over ${n} ticks, worst")

run(record 0 record-scripted "${replay}" ${match})
execute_process(COMMAND ${KFM_EMULATOR} "${KFM_REPLAY_EXE}" checksums "${replay}" ${crowd}
  OUTPUT_FILE "${sums}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "kfm_replay checksums failed (rc=${rc})")
endif()

run(verify 0 verify "${replay}" "${sums}" ${crowd})
expect_match("verify stderr" "${verify_err}" "^warning: ${line} tick ${n} with ${n}\n$")
expect_match("verify stdout" "${verify_out}" "^OK: 600 ticks, 601 checksums identical\n$")

run(verify_fail 1 verify "${replay}" "${sums}" ${crowd} --fail-on-dropped-contacts)
expect_match("verify --fail-on-dropped-contacts stderr" "${verify_fail_err}"
  "^error: ${line} tick ${n} with ${n}\n$")
expect_match("verify --fail-on-dropped-contacts stdout" "${verify_fail_out}" "^$")

run(bench 0 bench ${match})
expect_match("bench stderr" "${bench_err}" "^warning: ${line} match 0 tick ${n} with ${n}\n$")

run(stats 0 stats ${match} --matches 2 --quiet)
expect_match("stats stderr" "${stats_err}" "^warning: ${line} match [01] tick ${n} with ${n}\n$")

# The zero case on the shipped (embedded) content: no line anywhere.
string(REGEX REPLACE "\\.kfmr$" ".checksums" corpus_sums "${KFM_CORPUS_REPLAY}")
run(zero_verify 0 verify "${KFM_CORPUS_REPLAY}" "${corpus_sums}" --fail-on-dropped-contacts)
expect_match("corpus verify stderr" "${zero_verify_err}" "^$")
expect_match("corpus verify stdout" "${zero_verify_out}" "^OK: ${n} ticks, ${n} checksums identical\n$")
run(zero_bench 0 bench --ticks 600)
expect_match("default bench stderr" "${zero_bench_err}" "^$")
run(zero_stats 0 stats --players 4 --matches 1 --ticks 600 --quiet)
expect_match("default stats stderr" "${zero_stats_err}" "^$")
message(STATUS "contact-drop report: overflow reported, zero case silent")
