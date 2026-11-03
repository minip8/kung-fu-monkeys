# engine/net: message layer, transports, NetClient/NetServer (architecture
# §6). Depends on kfm::sim and kfm::core only, never on presentation,
# platform or ai (§4). No exceptions, no RTTI: fallible decoders return
# std::expected / bool. A real transport that wraps an exception-throwing
# library (libdatachannel) gets its own target that catches at the boundary.
add_library(kfm_net STATIC
  src/transport.cpp
  src/loopback.cpp
  src/messages.cpp
  src/input_history.cpp
)
add_library(kfm::net ALIAS kfm_net)

target_include_directories(kfm_net
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_net
  PUBLIC kfm::sim
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_net)

set_target_properties(kfm_net PROPERTIES FOLDER engine)
