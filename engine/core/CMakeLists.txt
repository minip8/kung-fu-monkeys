# engine/core: deterministic primitives. Depends on nothing but the standard
# library (architecture §4). No exceptions, no RTTI.
add_library(kfm_core STATIC
  src/assert.cpp
)
add_library(kfm::core ALIAS kfm_core)

target_include_directories(kfm_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(kfm_core PUBLIC cxx_std_${KFM_CXX_STANDARD})
target_link_libraries(kfm_core PRIVATE kfm::warnings)
kfm_no_exceptions_rtti(kfm_core)

set_target_properties(kfm_core PROPERTIES FOLDER engine)
