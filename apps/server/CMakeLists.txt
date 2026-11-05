# Dedicated server (architecture §3, §6, §13.5).
#
# kfm_server_host  ServerHost: the 60 Hz loop around one NetServer, its match
#                  restart policy, its bots (ai::AgentHost), logging and stats. Transport-agnostic and
#                  headless, so it is unit-tested over the loopback
#                  (tests/unit/server) on every target, wasm included.
# kfm_server       the CLI over WebRtcTransport. Native only (the browser
#                  cannot host the signaling socket) and only with
#                  KFM_WEBRTC; it builds in the server-headless preset.
#
# Both are built without exceptions or RTTI like the other apps: the
# transport catches at its own boundary.
add_library(kfm_server_host STATIC server_host.cpp)
add_library(kfm::server_host ALIAS kfm_server_host)
target_include_directories(kfm_server_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_server_host PUBLIC kfm::net kfm::ai PRIVATE kfm::warnings)
# The server's bots (§8): ai::AgentHost always, wasm modules only with
# KFM_WASM_BOTS (kfm::ai_wasm, native). engine/net never sees engine/ai; this
# library wires one into the other through NetServer's bot source.
if(TARGET kfm::ai_wasm)
  target_link_libraries(kfm_server_host PRIVATE kfm::ai_wasm)
  target_compile_definitions(kfm_server_host PRIVATE KFM_SERVER_WASM_BOTS=1)
endif()
kfm_no_exceptions_rtti(kfm_server_host)
set_target_properties(kfm_server_host PROPERTIES FOLDER apps)

if(NOT KFM_WEBRTC OR EMSCRIPTEN)
  return()
endif()

# kfm_server_lobby  LobbyAdapter: --lobby mode, net::LobbyLink wired to a
#                   ServerHost (tokens, room restarts, capacity, log lines);
#                   tested in tests/netcode against a fake lobby.
add_library(kfm_server_lobby STATIC server_lobby.cpp)
add_library(kfm::server_lobby ALIAS kfm_server_lobby)
target_include_directories(kfm_server_lobby PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kfm_server_lobby PUBLIC kfm::server_host kfm::net_webrtc
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_server_lobby)
set_target_properties(kfm_server_lobby PROPERTIES FOLDER apps)

add_executable(kfm_server main.cpp)
target_link_libraries(kfm_server PRIVATE kfm::server_host kfm::server_lobby kfm::net_webrtc
  kfm::net kfm::sim
  kfm::content kfm::warnings)
kfm_no_exceptions_rtti(kfm_server)
set_target_properties(kfm_server PROPERTIES FOLDER apps)
kfm_use_content(kfm_server)
