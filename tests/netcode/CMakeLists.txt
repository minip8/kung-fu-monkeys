# End-to-end netcode over the loopback emulator (architecture §6, §9): a
# NetServer and NetClients stepped in one deterministic loop (kfm/net/netsim.hpp, target kfm::netsim).
# The 60 s soaks are tagged [soak]; they run in the same executable.
kfm_add_test_exe(kfm_netcode_tests
  netcode_test.cpp
  session_test.cpp
)
target_link_libraries(kfm_netcode_tests PRIVATE kfm::netsim)

# The lobby seam (M6a, docs/lobby-protocol.md) over real sockets: a fake lobby
# (fake_lobby.hpp, an in-process rtc::WebSocketServer), kfm_server's lobby
# pieces and lobby-mode clients on 127.0.0.1. Labelled `network` like the
# other WebRTC tests.
if(TARGET kfm::server_lobby)
  kfm_add_test_exe(kfm_lobby_tests lobby_test.cpp)
  target_link_libraries(kfm_lobby_tests PRIVATE kfm::server_lobby kfm::net_webrtc kfm::datachannel)
  set_tests_properties(kfm_lobby_tests PROPERTIES LABELS network TIMEOUT 300)
endif()
