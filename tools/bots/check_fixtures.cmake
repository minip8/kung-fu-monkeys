# The bot_fixtures_up_to_date ctest: re-assembles every tests/fixtures/bots/*.wat
# with wabt's wat2wasm and fails when the checked-in .wasm beside it differs
# (regenerate with tools/bots/build_fixtures.sh). Prints "SKIPPED: wat2wasm"
# (the test's SKIP_REGULAR_EXPRESSION) when wat2wasm is not on PATH, as on
# the CI runners.
#   cmake -DFIXTURES_DIR=<tests/fixtures/bots> -DOUT_DIR=<scratch dir> -P check_fixtures.cmake
if(NOT FIXTURES_DIR OR NOT OUT_DIR)
  message(FATAL_ERROR "usage: cmake -DFIXTURES_DIR=... -DOUT_DIR=... -P check_fixtures.cmake")
endif()

find_program(KFM_WAT2WASM wat2wasm NO_CACHE)
if(NOT KFM_WAT2WASM)
  message("SKIPPED: wat2wasm (wabt) is not on PATH")
  return()
endif()

file(MAKE_DIRECTORY "${OUT_DIR}")
file(GLOB wats "${FIXTURES_DIR}/*.wat")
list(LENGTH wats count)
if(count EQUAL 0)
  message(FATAL_ERROR "no .wat fixtures in ${FIXTURES_DIR}")
endif()
set(stale "")
foreach(wat IN LISTS wats)
  get_filename_component(name "${wat}" NAME_WE)
  set(fresh "${OUT_DIR}/${name}.wasm")
  execute_process(COMMAND "${KFM_WAT2WASM}" "${wat}" -o "${fresh}"
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "wat2wasm failed on ${wat}:\n${err}")
  endif()
  set(checked_in "${FIXTURES_DIR}/${name}.wasm")
  if(NOT EXISTS "${checked_in}")
    list(APPEND stale "${name}.wasm (missing)")
    continue()
  endif()
  file(SHA256 "${fresh}" fresh_hash)
  file(SHA256 "${checked_in}" checked_in_hash)
  if(NOT fresh_hash STREQUAL checked_in_hash)
    list(APPEND stale "${name}.wasm")
  endif()
endforeach()
if(stale)
  list(JOIN stale ", " stale_list)
  message(FATAL_ERROR "bot fixtures out of date: ${stale_list}; "
                      "run tools/bots/build_fixtures.sh")
endif()
message("bot fixtures up to date: ${count} modules")
