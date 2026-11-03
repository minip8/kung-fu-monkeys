# End-to-end netcode over the loopback emulator (architecture §6, §9): a
# NetServer and NetClients stepped in one deterministic loop (netsim.hpp).
# The 60 s soaks are tagged [soak]; they run in the same executable.
kfm_add_test_exe(kfm_netcode_tests
  netcode_test.cpp
  session_test.cpp
)
target_link_libraries(kfm_netcode_tests PRIVATE kfm::net)
