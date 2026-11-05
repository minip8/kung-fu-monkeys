# kfm_netsim (architecture §4, §6.4, §9, §11): N clients and a server in one
# process over the loopback emulator, on the shared kfm::netsim scheduler.
# Headless and transport-free, so it builds everywhere (under Emscripten as a
# node CLI, like kfm_replay).
add_executable(kfm_netsim main.cpp)
target_link_libraries(kfm_netsim PRIVATE kfm::netsim kfm::content kfm::warnings)
kfm_no_exceptions_rtti(kfm_netsim)
set_target_properties(kfm_netsim PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # Node-only CLI: --csv writes host files, exit with the process code.
  target_link_options(kfm_netsim PRIVATE
    -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1MB)
endif()

# The M3 exit criterion as a gate (§11): 10 clients, no checksum mismatch or
# resync, bounded rollback depth. 30 simulated seconds each; label `soak`.
# (100 ms / 2 % and 300 ms / 10 % since M3; 500 ms / 5 % with ±30 ms jitter since v0.44.)
if(KFM_BUILD_TESTS)
  add_test(NAME netsim_10c_100ms_2pct
    COMMAND kfm_netsim --clients 10 --rtt 100 --jitter 10 --loss 2 --seconds 30 --quiet)
  add_test(NAME netsim_10c_300ms_10pct
    COMMAND kfm_netsim --clients 10 --rtt 300 --jitter 10 --loss 10 --seconds 30 --quiet)
  # v0.44: the high-RTT check (§6.2): 500 ms with ±30 ms jitter and 5% loss.
  add_test(NAME netsim_10c_500ms_5pct
    COMMAND kfm_netsim --clients 10 --rtt 500 --jitter 30 --loss 5 --seconds 30 --quiet)
  set_tests_properties(netsim_10c_100ms_2pct netsim_10c_300ms_10pct netsim_10c_500ms_5pct PROPERTIES
    LABELS soak
    TIMEOUT 300)
endif()
