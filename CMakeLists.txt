cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Toolchain selection (must precede project()).
# vcpkg is the dependency manager (architecture §10). If VCPKG_ROOT is set and
# no toolchain was given explicitly, use it. KFM_USE_VCPKG=OFF (the wasm
# preset) skips vcpkg entirely and dependencies come from FetchContent.
# ---------------------------------------------------------------------------
option(KFM_USE_VCPKG "Resolve dependencies through the vcpkg manifest" ON)
if(KFM_USE_VCPKG AND DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE FILEPATH "vcpkg toolchain")
  # Windows links its vcpkg dependencies statically (architecture §10). The
  # libdatachannel DLL's import library carries std::vector<std::byte>'s
  # members (MSVC exports the base-class specialisations of an exported
  # class), and every object file that uses a byte vector of its own then
  # collides with them at link time (LNK2005). The static library has no
  # dllimport side, and the port defines RTC_STATIC in its headers. `-md`
  # keeps the dynamic CRT, which is what CMake selects by default.
  if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET
     AND NOT DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
  endif()
endif()

project(KungFuMonkeys
  VERSION 0.0.1
  DESCRIPTION "Online arena brawler: monkeys, boomerang bananas, perfect dashes"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(KFM_BUILD_TESTS "Build unit, scenario and determinism tests" ON)
option(KFM_BUILD_APPS "Build the apps (replay runner, later client/server)" ON)
option(KFM_HEADLESS "Headless build: no presentation or platform layer" OFF)
option(KFM_CXX23_PROBE "Build the C++23 feature probe (architecture §9)" ON)
option(KFM_WEBRTC "Build the WebRTC transport (libdatachannel / datachannel-wasm, §6.4)" ON)
set(KFM_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")
set(KFM_CXX_STANDARD 23 CACHE STRING "C++ standard for all targets (architecture §9)")

# ---------------------------------------------------------------------------
# Global compile settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD ${KFM_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(KfmWarnings)
include(KfmSanitizers)
include(KfmDependencies)

if(KFM_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

# ---------------------------------------------------------------------------
# Modules. Layering is enforced through target link visibility (§4):
#   core -> sim -> {net, ai, presentation, content} -> apps
# ---------------------------------------------------------------------------
add_subdirectory(engine/core)
add_subdirectory(engine/sim)
add_subdirectory(engine/content)        # TOML content -> ContentDB; apps and tests only
add_subdirectory(engine/net)
add_subdirectory(engine/ai)             # IAgent, Observation, reference bot, WasmAgent (§8)
add_subdirectory(engine/presentation)   # headless-testable; the renderer target is skipped when KFM_HEADLESS
if(NOT KFM_HEADLESS)
  add_subdirectory(engine/platform)
endif()

if(KFM_BUILD_APPS)
  add_subdirectory(apps/replay)
  add_subdirectory(apps/net-probe)        # headless; needs KFM_WEBRTC
  add_subdirectory(apps/server)           # headless; kfm_server needs KFM_WEBRTC, native only
  add_subdirectory(apps/netsim)           # headless, loopback only
  if(NOT KFM_HEADLESS)
    add_subdirectory(apps/client)
    add_subdirectory(apps/render-probe)
  endif()
endif()

if(KFM_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(KFM_CXX23_PROBE)
  add_subdirectory(tools/cxx23-probe)
endif()
