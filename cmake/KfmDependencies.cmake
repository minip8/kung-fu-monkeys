# Third-party dependencies. vcpkg (manifest in vcpkg.json) is the primary
# source. When a package is not found (no vcpkg, or the wasm preset with
# KFM_USE_VCPKG=OFF) we fall back to FetchContent pinned to the same version
# as the manifest, so a build never silently uses a different release.

if(KFM_BUILD_TESTS)
  find_package(Catch2 3 CONFIG QUIET)
  if(Catch2_FOUND)
    message(STATUS "KFM: Catch2 ${Catch2_VERSION} from find_package")
  else()
    message(STATUS "KFM: Catch2 not found via find_package, fetching v3.16.1")
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG        v3.16.1
      GIT_SHALLOW    TRUE
      SYSTEM)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  endif()
  include(Catch)
endif()
