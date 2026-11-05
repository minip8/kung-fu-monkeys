# One test executable per module (kfm_add_test_exe is defined in tests/).
kfm_add_test_exe(kfm_core_tests
  core/fixed_test.cpp
  core/vec2_test.cpp
  core/pool_test.cpp
  core/ring_buffer_test.cpp
  core/bitstream_test.cpp
  core/rng_test.cpp
  core/hash_test.cpp
)
target_link_libraries(kfm_core_tests PRIVATE kfm::core)

kfm_add_test_exe(kfm_sim_tests
  sim/visit_test.cpp
  sim/replay_test.cpp
  sim/step_test.cpp
  sim/content_test.cpp
  sim/tags_test.cpp
  sim/input_test.cpp
  sim/ability_test.cpp
  sim/movement_test.cpp
  sim/projectile_test.cpp
  sim/collision_test.cpp
  sim/interaction_test.cpp
  sim/modifier_test.cpp
  sim/lifecycle_test.cpp
  sim/rules_test.cpp
  sim/statics_test.cpp
  sim/grid_cells_test.cpp
)
target_link_libraries(kfm_sim_tests PRIVATE kfm::sim kfm::sim_internal)

kfm_add_test_exe(kfm_content_tests
  content/decimal_test.cpp
  content/loader_test.cpp
  content/tilemap_test.cpp
)
target_link_libraries(kfm_content_tests PRIVATE kfm::content)
# The loader test reads the repo's content/ from disk: in place natively,
# embedded at /content in the wasm module under node.
if(EMSCRIPTEN)
  target_link_options(kfm_content_tests PRIVATE
    "SHELL:--embed-file \"${KFM_CONTENT_SOURCE_DIR}@/content\"")
  target_compile_definitions(kfm_content_tests PRIVATE KFM_CONTENT_DIR="/content")
else()
  target_compile_definitions(kfm_content_tests PRIVATE KFM_CONTENT_DIR="${KFM_CONTENT_SOURCE_DIR}")
endif()

# The transport contract suite (net/transport_contract.hpp) is header-only;
# a test executable for another transport links this target and expands
# KFM_TRANSPORT_CONTRACT_TESTS with its own harness.
add_library(kfm_transport_contract INTERFACE)
add_library(kfm::transport_contract ALIAS kfm_transport_contract)
target_include_directories(kfm_transport_contract INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/net)
target_link_libraries(kfm_transport_contract INTERFACE kfm::net Catch2::Catch2)

kfm_add_test_exe(kfm_net_tests
  net/transport_test.cpp
  net/loopback_test.cpp
  net/messages_test.cpp
  net/messages_fuzz_test.cpp
  net/input_history_test.cpp
  net/clock_sync_test.cpp
  net/event_journal_test.cpp
  net/net_server_test.cpp
  net/net_client_test.cpp
)
target_link_libraries(kfm_net_tests PRIVATE kfm::net kfm::transport_contract)

kfm_add_test_exe(kfm_ai_tests
  ai/agent_test.cpp
  ai/bot_spec_test.cpp
  ai/observation_test.cpp
  ai/agent_host_test.cpp
  ai/reference_bot_test.cpp
  ai/vision_test.cpp
)
target_link_libraries(kfm_ai_tests PRIVATE kfm::ai)

if(KFM_WASM_BOTS)
  # WasmAgent over WAMR (§8), on the hand-written modules in tests/fixtures/bots
  # (the .wasm files are checked in: CI has no wabt).
  set(KFM_BOT_FIXTURES_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/bots")
  kfm_add_test_exe(kfm_ai_wasm_tests
    ai/wasm_agent_test.cpp
    ai/example_bot_test.cpp
  )
  target_link_libraries(kfm_ai_wasm_tests PRIVATE kfm::ai_wasm kfm::sim_internal)
  target_compile_definitions(kfm_ai_wasm_tests PRIVATE KFM_BOT_FIXTURES_DIR="${KFM_BOT_FIXTURES_DIR}")
  # The C SDK seed (tools/bots/sdk-c/kfm_bot.h, compiled here as C++ against
  # the engine) and its example bots, built with tools/bots/build_example.sh
  # and checked in beside their source (CI has no wasm32 clang). SYSTEM: the
  # header is C, held to the C build's warnings, not this target's.
  target_include_directories(kfm_ai_wasm_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/tools/bots/sdk-c")
  target_compile_definitions(kfm_ai_wasm_tests PRIVATE
    KFM_BOT_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/tools/bots/examples")

  # Each fixture's .wasm matches its .wat (tools/bots/build_fixtures.sh
  # regenerates them); skipped where wabt's wat2wasm is not on PATH.
  add_test(NAME bot_fixtures_up_to_date
    COMMAND ${CMAKE_COMMAND}
      -DFIXTURES_DIR=${KFM_BOT_FIXTURES_DIR}
      -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/bot_fixtures
      -P ${PROJECT_SOURCE_DIR}/tools/bots/check_fixtures.cmake)
  set_tests_properties(bot_fixtures_up_to_date PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED: wat2wasm")
endif()

kfm_add_test_exe(kfm_presentation_tests
  presentation/math_test.cpp
  presentation/camera_test.cpp
  presentation/builder_test.cpp
  presentation/meshes_test.cpp
  presentation/animation_test.cpp
  presentation/audio_test.cpp
)
target_link_libraries(kfm_presentation_tests PRIVATE kfm::presentation)
if(NOT KFM_HEADLESS)
  # The glTF loader runs on the rig generated at build time (kfm_use_test_rig,
  # engine/presentation): the build-tree file natively, the copy embedded at
  # /content/models/test_rig.glb in the wasm module under node.
  target_sources(kfm_presentation_tests PRIVATE presentation/gltf_loader_test.cpp)
  target_link_libraries(kfm_presentation_tests PRIVATE kfm::presentation_gltf)
  kfm_use_test_rig(kfm_presentation_tests)
  if(EMSCRIPTEN)
    set(kfm_test_rig_path "/content/models/test_rig.glb")
  else()
    get_property(kfm_test_rig_path GLOBAL PROPERTY KFM_TEST_RIG_GLB)
  endif()
  target_compile_definitions(kfm_presentation_tests PRIVATE
    KFM_TEST_RIG_PATH="${kfm_test_rig_path}")
endif()

if(NOT KFM_HEADLESS)
  kfm_add_test_exe(kfm_platform_tests
    platform/input_sampler_test.cpp
    platform/platform_services_test.cpp
  )
  target_link_libraries(kfm_platform_tests PRIVATE kfm::platform_input kfm::platform_services)
endif()

if(NOT KFM_HEADLESS)
  # apps/client: the headless match loop (kfm_client_core) and the SDL event
  # translation (kfm_client_sdl); no window is opened.
  kfm_add_test_exe(kfm_client_tests
    client/fixed_step_clock_test.cpp
    client/match_session_test.cpp
    client/content_tuning_test.cpp
    client/sdl_input_test.cpp
    client/net_session_test.cpp
    client/event_fx_rollback_test.cpp
    client/lobby_test.cpp
    client/bot_session_test.cpp
    client/room_test.cpp
  )
  target_link_libraries(kfm_client_tests PRIVATE kfm::client_core kfm::client_sdl kfm::content)
  if(KFM_WASM_BOTS)
    # Bot modules as local players: the session loads the same fixtures.
    target_compile_definitions(kfm_client_tests PRIVATE
      KFM_BOT_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/bots")
  endif()

  if(KFM_WEBRTC AND NOT EMSCRIPTEN)
    # The client's network mode over real sockets: NetServer on
    # WebRtcTransport::createServer and a NetSession on createClient, one
    # process, 127.0.0.1. Labelled `network` like the other WebRTC tests.
    kfm_add_test_exe(kfm_client_net_tests client/net_session_webrtc_test.cpp)
    target_link_libraries(kfm_client_net_tests PRIVATE kfm::client_core kfm::net_webrtc)
    set_tests_properties(kfm_client_net_tests PROPERTIES LABELS network TIMEOUT 120)
  endif()
endif()

if(KFM_WEBRTC AND NOT EMSCRIPTEN)
  # WebRtcTransport over real libdatachannel on 127.0.0.1 (no internet:
  # host candidates only). Labelled `network` so it can be excluded with
  # `ctest -LE network`; a generous timeout because ICE + DTLS per test case
  # is slow under sanitizers.
  kfm_add_test_exe(kfm_net_webrtc_tests
    net/webrtc_signaling_test.cpp
    net/webrtc_api_test.cpp
    net/webrtc_transport_test.cpp
    net/webrtc_contract_test.cpp
    net/webrtc_external_test.cpp
  )
  target_include_directories(kfm_net_webrtc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(kfm_net_webrtc_tests PRIVATE kfm::net_webrtc kfm::transport_contract)
  set_tests_properties(kfm_net_webrtc_tests PROPERTIES LABELS network TIMEOUT 300)
endif()

if(KFM_WEBRTC AND EMSCRIPTEN)
  # The browser transport links (datachannel-wasm's --js-library files, wasm
  # exceptions) and its browser-free paths run under node.
  kfm_add_test_exe(kfm_net_webrtc_tests
    net/webrtc_signaling_test.cpp
    net/webrtc_api_test.cpp
  )
  target_link_libraries(kfm_net_webrtc_tests PRIVATE kfm::net_webrtc)
endif()

if(TARGET kfm::server_host)
  # apps/server's ServerHost over the loopback emulator: match restart,
  # stats lines, shutdown, the v0.42 room flow (every target, wasm included);
  # the Room state machine on its own.
  kfm_add_test_exe(kfm_server_tests
    server/server_host_test.cpp
    server/room_test.cpp
    server/room_flow_test.cpp
  )
  target_link_libraries(kfm_server_tests PRIVATE kfm::server_host kfm::netsim)
  if(KFM_WASM_BOTS)
    # The host's wasm bots on the fixture modules (echo, trap, abi_mismatch).
    target_compile_definitions(kfm_server_tests PRIVATE KFM_SERVER_TEST_WASM=1
      KFM_BOT_FIXTURES_DIR="${KFM_BOT_FIXTURES_DIR}")
  endif()

  if(TARGET kfm_server)
    # The same over real sockets: WebRtcTransport on 127.0.0.1, two NetClients
    # in-process on the steady clock (label `network`, like the transport tests).
    kfm_add_test_exe(kfm_server_network_tests
      server/server_webrtc_test.cpp
    )
    target_link_libraries(kfm_server_network_tests PRIVATE kfm::server_host kfm::net_webrtc)
    set_tests_properties(kfm_server_network_tests PROPERTIES LABELS network TIMEOUT 120)
  endif()
endif()

if(TARGET kfm::arena_core)
  # apps/arena's match runner (§8, §9): determinism, placements, the result
  # line and JSON, replays; with KFM_WASM_BOTS the module path over the
  # fixtures in tests/fixtures/bots.
  kfm_add_test_exe(kfm_arena_tests
    arena/arena_test.cpp
  )
  target_link_libraries(kfm_arena_tests PRIVATE kfm::arena_core kfm::content)
  if(KFM_WASM_BOTS)
    target_compile_definitions(kfm_arena_tests PRIVATE KFM_WASM_BOTS=1
      KFM_BOT_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/bots")
  endif()
endif()
