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
)
target_link_libraries(kfm_sim_tests PRIVATE kfm::sim kfm::sim_internal)

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

kfm_add_test_exe(kfm_presentation_tests
  presentation/math_test.cpp
  presentation/camera_test.cpp
  presentation/builder_test.cpp
  presentation/meshes_test.cpp
  presentation/animation_test.cpp
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
  )
  target_link_libraries(kfm_platform_tests PRIVATE kfm::platform_input)
endif()

if(NOT KFM_HEADLESS)
  # apps/client: the headless match loop (kfm_client_core) and the SDL event
  # translation (kfm_client_sdl); no window is opened.
  kfm_add_test_exe(kfm_client_tests
    client/fixed_step_clock_test.cpp
    client/match_session_test.cpp
    client/content_tuning_test.cpp
    client/sdl_input_test.cpp
  )
  target_link_libraries(kfm_client_tests PRIVATE kfm::client_core kfm::client_sdl)
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
  )
  target_include_directories(kfm_net_webrtc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(kfm_net_webrtc_tests PRIVATE kfm::net_webrtc)
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
