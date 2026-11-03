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
  src/clock_sync.cpp
  src/event_journal.cpp
  src/net_server.cpp
  src/net_client.cpp
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

# ---------------------------------------------------------------------------
# kfm_net_webrtc: WebRtcTransport, the production ITransport (§6.4), over
# libdatachannel natively and datachannel-wasm in the browser (kfm::datachannel,
# cmake/KfmDependencies.cmake). A separate target because the rtc:: API
# throws: this one is compiled WITH exceptions (no kfm_no_exceptions_rtti)
# and catches everything at its boundary, so kfm_net and the apps stay
# exception-free. Its public headers include nothing from rtc::.
# ---------------------------------------------------------------------------
if(KFM_WEBRTC)
  add_library(kfm_net_webrtc STATIC
    src/webrtc/signaling.cpp
    src/webrtc/webrtc_transport.cpp
  )
  add_library(kfm::net_webrtc ALIAS kfm_net_webrtc)
  target_include_directories(kfm_net_webrtc
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # kfm::net only for the ITransport seam it implements.
  target_link_libraries(kfm_net_webrtc
    PUBLIC kfm::net
    PRIVATE kfm::datachannel kfm::warnings)
  if(EMSCRIPTEN)
    # Real try/catch around datachannel-wasm (Emscripten's default turns a
    # throw into an abort); kfm::datachannel adds the matching link flag.
    target_compile_options(kfm_net_webrtc PRIVATE -fwasm-exceptions)
    # datachannel-wasm's JS library assumes an older Emscripten runtime,
    # which only shows in a browser (the node tests never reach it):
    #   - it calls _malloc/_free without declaring them; the extra JS library
    #     pulls them in (see the file);
    #   - it reads Module['HEAPU8'] / Module['HEAPU32'], which Emscripten 4+
    #     only sets (and refreshes on memory growth) when they are exported.
    #     An executable that sets its own EXPORTED_RUNTIME_METHODS (the
    #     setting is last-wins) must keep these two in its list.
    set(kfm_datachannel_deps_js "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc/datachannel_wasm_deps.js")
    target_link_options(kfm_net_webrtc INTERFACE
      "SHELL:--js-library \"${kfm_datachannel_deps_js}\""
      "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32")
    set_property(TARGET kfm_net_webrtc APPEND PROPERTY INTERFACE_LINK_DEPENDS
      "${kfm_datachannel_deps_js}")
  endif()
  set_target_properties(kfm_net_webrtc PROPERTIES FOLDER engine)
endif()
