# engine/content: TOML content files -> validated sim::ContentDB
# (architecture §4, §5.10). Above sim, beside net/ai/presentation, which
# never depend on it; only apps (and tests) do. The only engine module that
# reads files. toml++ is a PRIVATE dependency: the public API speaks
# ContentDB, ContentSource and Diagnostics.
set(KFM_CONTENT_SOURCE_DIR "${PROJECT_SOURCE_DIR}/content")
file(GLOB_RECURSE kfm_content_files CONFIGURE_DEPENDS "${KFM_CONTENT_SOURCE_DIR}/*.toml")
set(kfm_content_embedded "${CMAKE_CURRENT_BINARY_DIR}/embedded.cpp")
add_custom_command(
  OUTPUT "${kfm_content_embedded}"
  COMMAND ${CMAKE_COMMAND}
    "-DKFM_CONTENT_DIR=${KFM_CONTENT_SOURCE_DIR}"
    "-DKFM_OUT=${kfm_content_embedded}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedContent.cmake"
  DEPENDS ${kfm_content_files} "${CMAKE_CURRENT_SOURCE_DIR}/EmbedContent.cmake"
  COMMENT "Embedding content/*.toml"
  VERBATIM)

add_library(kfm_content STATIC
  src/decimal.cpp
  src/loader.cpp
  src/app_content.cpp
  "${kfm_content_embedded}"
)
add_library(kfm::content ALIAS kfm_content)

target_include_directories(kfm_content
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kfm_content
  PUBLIC kfm::sim
  PRIVATE kfm::tomlplusplus kfm::warnings)
kfm_no_exceptions_rtti(kfm_content)
set_target_properties(kfm_content PROPERTIES FOLDER engine)

# The repo's content/ for tests that load the real files from disk, and the
# copy script kfm_use_content() runs (the function body is expanded in the
# caller's scope).
set(KFM_CONTENT_SOURCE_DIR "${KFM_CONTENT_SOURCE_DIR}" PARENT_SCOPE)
set(KFM_CONTENT_COPY_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/CopyContent.cmake" PARENT_SCOPE)

# kfm_use_content(<target>): copies content/ next to an executable
# (<its directory>/content/*.toml, beside the test rig's content/models/)
# after every link, so a native build finds it without --content and can be
# re-tuned without a rebuild. Natively only: the browser build reads the
# embedded copy. Every host relinks when the TOML changes (embedded.cpp),
# which refreshes the copy.
function(kfm_use_content target)
  if(EMSCRIPTEN)
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DKFM_CONTENT_DIR=${KFM_CONTENT_SOURCE_DIR}"
      "-DKFM_DEST=$<TARGET_FILE_DIR:${target}>/content"
      -P "${KFM_CONTENT_COPY_SCRIPT}"
    VERBATIM)
endfunction()
