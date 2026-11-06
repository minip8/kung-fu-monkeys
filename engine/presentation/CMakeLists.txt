# engine/presentation: reads sim state, never writes it (architecture §4, §7).
# kfm_presentation is the headless part (RenderState derivation, interpolation,
# smoothing, camera math, procedural meshes, skinned-model data and the
# animation layer) and is unit-tested without a GPU. The glTF loader (cgltf)
# and the sokol_gfx renderer are separate targets added below it and are
# skipped in KFM_HEADLESS builds.
add_library(kfm_presentation STATIC
  src/math.cpp
  src/camera.cpp
  src/builder.cpp
  src/meshes.cpp
  src/skinned_model.cpp
  src/animation.cpp
  src/event_fx.cpp
  src/sound_bank.cpp
  src/mixer.cpp
  src/theme.cpp
  src/ground.cpp
)
add_library(kfm::presentation ALIAS kfm_presentation)

target_include_directories(kfm_presentation
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_presentation
  PUBLIC kfm::sim
  PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_presentation)

set_target_properties(kfm_presentation PROPERTIES FOLDER engine)

if(NOT KFM_HEADLESS)
  # --- glTF loading ----------------------------------------------------------
  # The cgltf implementation TU in its own target, compiled without
  # kfm::warnings (third-party code), and the loader on top of it.
  add_library(kfm_cgltf_impl STATIC src/cgltf_impl.cpp)
  target_link_libraries(kfm_cgltf_impl PRIVATE kfm::cgltf)
  kfm_no_exceptions_rtti(kfm_cgltf_impl)
  set_target_properties(kfm_cgltf_impl PROPERTIES FOLDER third_party)

  add_library(kfm_presentation_gltf STATIC src/gltf_loader.cpp)
  add_library(kfm::presentation_gltf ALIAS kfm_presentation_gltf)
  target_link_libraries(kfm_presentation_gltf
    PUBLIC kfm::presentation
    PRIVATE kfm::cgltf kfm_cgltf_impl kfm::warnings)
  kfm_no_exceptions_rtti(kfm_presentation_gltf)
  set_target_properties(kfm_presentation_gltf PROPERTIES FOLDER engine)

  # --- the rigged test model -------------------------------------------------
  # M2 stand-in for the Blender -> glTF pipeline (§7): tools/assets/
  # gen_test_rig.py writes the model at build time; exported glTF is never
  # checked in. kfm_use_test_rig(<target>) makes an executable find it at
  # <its directory>/content/models/test_rig.glb natively (copied next to it
  # after every link) and at /content/models/test_rig.glb under Emscripten
  # (embedded in the module's virtual file system).
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(kfm_test_rig_script "${PROJECT_SOURCE_DIR}/tools/assets/gen_test_rig.py")
  set(kfm_test_rig_glb "${PROJECT_BINARY_DIR}/content/models/test_rig.glb")
  add_custom_command(
    OUTPUT "${kfm_test_rig_glb}"
    COMMAND Python3::Interpreter "${kfm_test_rig_script}" --out "${kfm_test_rig_glb}"
    DEPENDS "${kfm_test_rig_script}"
    COMMENT "Generating the rigged test model content/models/test_rig.glb"
    VERBATIM)
  add_custom_target(kfm_test_rig ALL DEPENDS "${kfm_test_rig_glb}")
  set_target_properties(kfm_test_rig PROPERTIES FOLDER content)
  set_property(GLOBAL PROPERTY KFM_TEST_RIG_GLB "${kfm_test_rig_glb}")

  function(kfm_use_test_rig target)
    get_property(glb GLOBAL PROPERTY KFM_TEST_RIG_GLB)
    add_dependencies(${target} kfm_test_rig)
    # Relink (and so re-copy or re-embed) whenever the model changes.
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${glb}")
    if(EMSCRIPTEN)
      target_link_options(${target} PRIVATE
        "SHELL:--embed-file \"${glb}@/content/models/test_rig.glb\"")
    else()
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/content/models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${glb}"
                "$<TARGET_FILE_DIR:${target}>/content/models/test_rig.glb"
        VERBATIM)
    endif()
  endfunction()

  # --- the sokol_gfx renderer ------------------------------------------------
  # Presentation code that needs a GPU. Links kfm::sokol (the sokol_gfx target
  # from cmake/KfmDependencies.cmake), never kfm::platform.
  add_library(kfm_renderer STATIC
    renderer/renderer.cpp
  )
  add_library(kfm::renderer ALIAS kfm_renderer)
  target_include_directories(kfm_renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer)
  target_link_libraries(kfm_renderer
    PUBLIC kfm::presentation
    PRIVATE kfm::presentation_gltf kfm::sokol kfm::warnings)
  kfm_no_exceptions_rtti(kfm_renderer)
  set_target_properties(kfm_renderer PROPERTIES FOLDER engine)
endif()
