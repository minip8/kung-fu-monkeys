# WebRTC probe (architecture §6.4, M3): WebRtcTransport end to end between a
# native echo server and a native or browser client. Headless: no SDL, GL or
# ImGui, only kfm::net and kfm::net_webrtc.
#
#   native  kfm_net_probe --serve PORT | --connect ws://HOST:PORT | --loopback
#   wasm    kfm_net_probe.html, the client side only
#           (?connect=ws://127.0.0.1:9000&messages=200), driven by
#           tools/web/check_webrtc.py in headless Chromium. Never a ctest.
#
# Built without exceptions like the client: the transport catches at its own
# boundary, so the probe also proves an exception-free caller links it.
if(NOT KFM_WEBRTC)
  return()
endif()

set(kfm_net_probe_sources main.cpp probe_client.cpp)
if(NOT EMSCRIPTEN)
  list(APPEND kfm_net_probe_sources echo_server.cpp)  # the server role is native only
endif()
add_executable(kfm_net_probe ${kfm_net_probe_sources})
target_link_libraries(kfm_net_probe PRIVATE kfm::net_webrtc kfm::net kfm::warnings)
kfm_no_exceptions_rtti(kfm_net_probe)
set_target_properties(kfm_net_probe PROPERTIES FOLDER apps)

if(EMSCRIPTEN)
  # kfm::datachannel adds -fwasm-exceptions and datachannel-wasm's two
  # --js-library files to the link.
  set_target_properties(kfm_net_probe PROPERTIES SUFFIX ".html")
  set(kfm_net_probe_shell "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
  target_link_options(kfm_net_probe PRIVATE
    --shell-file "${kfm_net_probe_shell}"
    -sSTACK_SIZE=1MB
    -sALLOW_MEMORY_GROWTH=1
    -sENVIRONMENT=web)
  set_property(TARGET kfm_net_probe APPEND PROPERTY LINK_DEPENDS "${kfm_net_probe_shell}")
endif()

# The probe's server and client against each other in one process on
# 127.0.0.1 (host candidates only, no internet). Labelled like the other
# real-network tests so `ctest -LE network` skips it.
if(KFM_BUILD_TESTS AND NOT EMSCRIPTEN)
  add_test(NAME net_probe_loopback
    COMMAND $<TARGET_FILE:kfm_net_probe> --loopback --messages 200 --timeout-seconds 60)
  set_tests_properties(net_probe_loopback PROPERTIES
    LABELS network
    TIMEOUT 120
    RUN_SERIAL TRUE)
endif()
