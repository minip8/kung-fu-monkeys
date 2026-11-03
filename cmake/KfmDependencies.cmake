# Third-party dependencies. vcpkg (manifest in vcpkg.json) is the primary
# source. When a package is not found (no vcpkg, or the wasm preset with
# KFM_USE_VCPKG=OFF) we fall back to FetchContent pinned to the same version
# as the manifest, so a build never silently uses a different release.
#
# Targets defined or normalised here (architecture §7, §10):
#   SDL3::SDL3   SDL 3.4.18 (vcpkg sdl3, or FetchContent release-3.4.18 as a static library)
#   kfm::imgui   Dear ImGui 1.93.0 core + the SDL3 platform backend (FetchContent only)
#   kfm::glad    glad2 GL 4.3 core loader vendored in third_party/glad (native only)
#   kfm::sokol   sokol_gfx + sokol_log + util/sokol_imgui, one implementation TU (FetchContent only)
#   kfm::cgltf   cgltf 1.15 (header only; the includer that defines CGLTF_IMPLEMENTATION owns it)
#   kfm::datachannel  libdatachannel 0.24.6 natively (vcpkg, or FetchContent static with OpenSSL),
#                datachannel-wasm f9200eb under Emscripten; only when KFM_WEBRTC (headless too)
#   kfm::tomlplusplus  toml++ 3.4.0 (header only, TOML_EXCEPTIONS=0), for engine/content (headless too)
# None of them link kfm::warnings: third-party code is compiled with its own
# flags and its headers are SYSTEM includes, so it never trips -Werror.

include(FetchContent)

if(KFM_BUILD_TESTS)
  find_package(Catch2 3 CONFIG QUIET)
  if(Catch2_FOUND)
    message(STATUS "KFM: Catch2 ${Catch2_VERSION} from find_package")
  else()
    message(STATUS "KFM: Catch2 not found via find_package, fetching v3.16.1")
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

# ---------------------------------------------------------------------------
# WebRTC data channels (architecture §6.4): kfm::datachannel, the C++ `rtc::`
# API used by kfm_net_webrtc. Defined before the headless return below
# because the dedicated server needs it. KFM_WEBRTC=OFF skips it entirely
# (no network dependencies; kfm_net_webrtc and its tests are not built).
#
#   native      libdatachannel 0.24.6 (vcpkg `libdatachannel[ws]` without
#               default features, i.e. no `srtp` media transport), or the
#               FetchContent fallback below, built static with OpenSSL as
#               the TLS backend (DTLS for the data channels; libssl-dev on
#               Linux, see .github/workflows/ci.yml), libjuice for ICE and
#               usrsctp for SCTP, WebSocket support on, media off.
#   Emscripten  datachannel-wasm (same `rtc::` API over the browser's
#               RTCPeerConnection/WebSocket), FetchContent only, pinned to
#               master f9200eb (2026-09-27: v0.4.0 plus a missing
#               <functional> include). It links two --js-library files.
# ---------------------------------------------------------------------------
if(KFM_WEBRTC)
  add_library(kfm_datachannel INTERFACE)
  add_library(kfm::datachannel ALIAS kfm_datachannel)
  if(EMSCRIPTEN)
    message(STATUS "KFM: datachannel-wasm f9200eb from FetchContent")
    FetchContent_Declare(datachannel_wasm
      URL      https://github.com/paullouisageneau/datachannel-wasm/archive/f9200eb942f66ddbfecf088ce7122d095aca1c3e.tar.gz
      URL_HASH SHA256=3271291205c5b3ef06a7a6cbfe107cab93ecfd8b3a303afd94337e02b0343349
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SYSTEM)
    # Its cmake_minimum_required(3.7) draws CMake 4's deprecation warning;
    # nothing in it depends on pre-3.10 policies.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    FetchContent_MakeAvailable(datachannel_wasm)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    # datachannel-wasm reports errors by throwing (std::runtime_error);
    # kfm_net_webrtc catches them at its boundary. Native wasm exception
    # handling on both sides, and on whatever links kfm::datachannel, so a
    # throw is catchable instead of aborting (Emscripten's default).
    target_compile_options(datachannel-wasm PRIVATE -fwasm-exceptions)
    target_link_libraries(kfm_datachannel INTERFACE datachannel-wasm)
    target_link_options(kfm_datachannel INTERFACE -fwasm-exceptions)
    set_target_properties(datachannel-wasm PROPERTIES FOLDER third_party)
  else()
    find_package(LibDataChannel 0.24.6 EXACT CONFIG QUIET)
    if(LibDataChannel_FOUND)
      message(STATUS "KFM: libdatachannel ${LibDataChannel_VERSION} from find_package")
      # vcpkg builds the static library under the `LibDataChannel` export
      # name on static triplets (its `-static` twin is never installed).
      if(TARGET LibDataChannel::LibDataChannelStatic)
        target_link_libraries(kfm_datachannel INTERFACE LibDataChannel::LibDataChannelStatic)
      else()
        target_link_libraries(kfm_datachannel INTERFACE LibDataChannel::LibDataChannel)
      endif()
    else()
      message(STATUS "KFM: libdatachannel not found via find_package, fetching v0.24.6")
      # The release tarball has empty deps/ submodule directories, so each
      # submodule libdatachannel needs (plog, usrsctp, libjuice; json and
      # libsrtp only serve the examples and media) is fetched as its own
      # tarball at the commit v0.24.6 pins and extracted into deps/. A git
      # clone with submodules would do the same without the hashes. The
      # outer tarball is extracted first and its deps then filled in; bumping
      # the pin means deleting build/<preset>/_deps (the check below says so).
      FetchContent_Declare(libdatachannel
        URL      https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.6.tar.gz
        URL_HASH SHA256=22a66aebebd8dae561d8e5181da96f2fcc203e067c5b2186c07b39af913464c0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR  kfm-deferred)  # added below with our options, not by MakeAvailable
      FetchContent_MakeAvailable(libdatachannel)
      FetchContent_Declare(libdatachannel_plog
        URL      https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz
        URL_HASH SHA256=92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR     ${libdatachannel_SOURCE_DIR}/deps/plog
        SOURCE_SUBDIR  kfm-deferred)
      FetchContent_Declare(libdatachannel_usrsctp
        URL      https://github.com/paullouisageneau/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz
        URL_HASH SHA256=e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR     ${libdatachannel_SOURCE_DIR}/deps/usrsctp
        SOURCE_SUBDIR  kfm-deferred)
      FetchContent_Declare(libdatachannel_libjuice
        URL      https://github.com/paullouisageneau/libjuice/archive/b89c792e3612faf2f12cf35bcc56857313a06be3.tar.gz
        URL_HASH SHA256=713b22174e8cb2e06538fa9bac2650bda21b83c86f5128813ab4dcf8f5bea9ed
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR     ${libdatachannel_SOURCE_DIR}/deps/libjuice
        SOURCE_SUBDIR  kfm-deferred)
      FetchContent_MakeAvailable(libdatachannel_plog libdatachannel_usrsctp libdatachannel_libjuice)
      foreach(dep plog usrsctp libjuice)
        if(NOT EXISTS "${libdatachannel_SOURCE_DIR}/deps/${dep}/CMakeLists.txt")
          message(FATAL_ERROR "libdatachannel deps/${dep} is empty (the pin changed?): "
                              "delete ${FETCHCONTENT_BASE_DIR} and configure again")
        endif()
      endforeach()

      # A function scope keeps these switches away from the rest of the
      # build; all three projects honour normal variables over option()
      # (CMP0077 NEW at their minimum versions).
      function(kfm_add_libdatachannel)
        set(BUILD_SHARED_LIBS OFF)
        set(NO_EXAMPLES ON)
        set(NO_TESTS ON)          # libdatachannel and libjuice
        set(NO_MEDIA ON)          # data channels only: no libsrtp
        set(NO_WEBSOCKET OFF)     # the signaling socket (client and server)
        set(USE_GNUTLS OFF)       # OpenSSL
        set(USE_MBEDTLS OFF)
        set(USE_NICE OFF)         # libjuice
        set(PREFER_SYSTEM_LIB OFF)
        set(WARNINGS_AS_ERRORS OFF)
        set(NO_SERVER ON)         # libjuice's embedded TURN server
        set(sctp_werror OFF)
        set(sctp_debug OFF)
        set(sctp_build_programs OFF)
        set(sctp_build_shared_lib OFF)
        add_subdirectory(${libdatachannel_SOURCE_DIR} ${libdatachannel_BINARY_DIR}
          EXCLUDE_FROM_ALL SYSTEM)
      endfunction()
      kfm_add_libdatachannel()
      target_link_libraries(kfm_datachannel INTERFACE LibDataChannel::LibDataChannelStatic)
      foreach(t datachannel datachannel-static juice juice-static usrsctp)
        if(TARGET ${t})
          set_target_properties(${t} PROPERTIES FOLDER third_party)
        endif()
      endforeach()
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# toml++ 3.4.0 (header only), the content file parser (engine/content,
# architecture §5.10). Every host loads content, so the headless server
# needs it too. kfm::tomlplusplus pins the configuration every includer must
# share: no exceptions (parse errors come back as toml::parse_result, which
# the loader turns into diagnostics) and no formatters (never printed back).
# ---------------------------------------------------------------------------
find_package(tomlplusplus 3.4.0 EXACT CONFIG QUIET)
if(tomlplusplus_FOUND)
  message(STATUS "KFM: toml++ ${tomlplusplus_VERSION} from find_package")
else()
  message(STATUS "KFM: toml++ not found via find_package, fetching v3.4.0")
  FetchContent_Declare(tomlplusplus
    URL      https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  kfm-no-cmake)  # header only: the include directory is all we use
  FetchContent_MakeAvailable(tomlplusplus)
  add_library(tomlplusplus_tomlplusplus INTERFACE)
  target_include_directories(tomlplusplus_tomlplusplus SYSTEM INTERFACE
    ${tomlplusplus_SOURCE_DIR}/include)
  add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus_tomlplusplus)
endif()
add_library(kfm_tomlplusplus INTERFACE)
target_link_libraries(kfm_tomlplusplus INTERFACE tomlplusplus::tomlplusplus)
target_compile_definitions(kfm_tomlplusplus INTERFACE
  TOML_EXCEPTIONS=0 TOML_ENABLE_FORMATTERS=0 TOML_HEADER_ONLY=1)
add_library(kfm::tomlplusplus ALIAS kfm_tomlplusplus)

# Everything below is the client stack (window, GPU, debug UI, assets). The
# headless server build never needs it.
if(KFM_HEADLESS)
  return()
endif()

# SDL3 and glad are C; the project itself only declares CXX.
enable_language(C)

set(KFM_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party")
cmake_path(NORMAL_PATH KFM_THIRD_PARTY_DIR)

# ---------------------------------------------------------------------------
# SDL3 3.4.18
# ---------------------------------------------------------------------------
# EXACT: a distro libsdl3-dev of another release must not be picked up
# silently. vcpkg's port version is not part of SDL3_VERSION, so the vcpkg
# sdl3 3.4.18 port matches.
find_package(SDL3 3.4.18 EXACT CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "KFM: SDL3 ${SDL3_VERSION} from find_package")
else()
  message(STATUS "KFM: SDL3 not found via find_package, fetching release-3.4.18")
  FetchContent_Declare(SDL3
    URL      https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
    URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
  # Mirror the vcpkg sdl3 port at the features vcpkg.json selects (x11 and
  # wayland on Linux, no ibus/dbus/alsa/vulkan) so both paths build the same
  # SDL. The port also switches off every optional host library that would
  # silently change SDL's capabilities from one machine to the next; SDL's
  # own CMake otherwise fails on Linux when any enabled one is missing.
  foreach(opt
      SDL_DBUS SDL_IBUS SDL_ALSA SDL_VULKAN SDL_LIBUDEV SDL_LIBURING
      SDL_FRIBIDI SDL_JACK SDL_KMSDRM SDL_LIBTHAI SDL_PIPEWIRE SDL_PULSEAUDIO
      SDL_ROCKCHIP SDL_RPI SDL_SNDIO SDL_WAYLAND_LIBDECOR
      SDL_X11_XCURSOR SDL_X11_XDBE SDL_X11_XFIXES SDL_X11_XINPUT SDL_X11_XRANDR
      SDL_X11_XSHAPE SDL_X11_XSCRNSAVER SDL_X11_XSYNC SDL_X11_XTEST)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  set(SDL_REVISION "release-3.4.18" CACHE STRING "" FORCE)
  FetchContent_MakeAvailable(SDL3)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui 1.93.0 core + the SDL3 platform backend. Rendering goes through
# sokol_imgui, so no ImGui renderer backend is built.
#
# FetchContent on every path, not vcpkg: the vcpkg `imgui[sdl3-binding]`
# feature depends on `sdl3` with its default features, which re-enables
# ibus + dbus on Linux despite our `default-features: false` and drags in
# libsystemd, meson and an autotools build of libxcrypt. ImGui has no build
# system upstream (six source files), so building it here at the pinned tag is
# simpler and identical everywhere.
# ---------------------------------------------------------------------------
message(STATUS "KFM: Dear ImGui v1.93.0 from FetchContent")
FetchContent_Declare(imgui
  URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.tar.gz
  URL_HASH SHA256=f7bf6bc76e5d79d40d751096663e6c5d18daf08129dd52e7f7b09e4251c8eeee
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)  # no CMakeLists upstream: sources only
add_library(kfm_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
target_include_directories(kfm_imgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(kfm_imgui PUBLIC SDL3::SDL3)
set_target_properties(kfm_imgui PROPERTIES FOLDER third_party)
add_library(kfm::imgui ALIAS kfm_imgui)

# ---------------------------------------------------------------------------
# glad2: GL 4.3 core function loader, generated and vendored (third_party/glad;
# its README says why 4.3 and not 4.1). sokol_gfx runs with SOKOL_EXTERNAL_GL_LOADER natively so no
# system GL headers (GL/gl.h, OpenGL/gl3.h) are needed to build. Emscripten
# uses its own GLES3 headers instead.
# ---------------------------------------------------------------------------
if(NOT EMSCRIPTEN)
  add_library(kfm_glad STATIC ${KFM_THIRD_PARTY_DIR}/glad/src/gl.c)
  target_include_directories(kfm_glad SYSTEM PUBLIC ${KFM_THIRD_PARTY_DIR}/glad/include)
  set_target_properties(kfm_glad PROPERTIES FOLDER third_party)
  add_library(kfm::glad ALIAS kfm_glad)
endif()

# ---------------------------------------------------------------------------
# sokol (sokol_gfx.h, sokol_log.h, util/sokol_imgui.h). FetchContent only: the
# vcpkg `sokol` port is a 2023-10-07 snapshot, years behind the API we use and
# without ImGui 1.92+ texture support. Pinned to floooh/sokol master at
# efeb776 (2026-10-17, "sokol-imgui-fix-1.93.0", the fix for ImGui 1.93.0).
#
# kfm::sokol is its own target, not part of kfm_platform, because both
# kfm_platform and the renderer in engine/presentation use sokol_gfx and
# presentation must never depend on platform (§4). The backend define is
# PUBLIC so every includer of sokol_gfx.h sees the same declarations.
# ---------------------------------------------------------------------------
FetchContent_Declare(sokol
  URL      https://github.com/floooh/sokol/archive/efeb77603e24daea7d1dd6445b9358bb5d82533b.tar.gz
  URL_HASH SHA256=0500b6bf1e10e35a85c28435ec27bb06dbad0948536e5ea37554453842aa0f0f
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sokol)  # no CMakeLists upstream: headers only

add_library(kfm_sokol STATIC ${KFM_THIRD_PARTY_DIR}/sokol/sokol_impl.cpp)
target_include_directories(kfm_sokol SYSTEM PUBLIC ${sokol_SOURCE_DIR})
target_compile_definitions(kfm_sokol PUBLIC SOKOL_IMGUI_NO_SOKOL_APP)
if(EMSCRIPTEN)
  # WebGL2 via Emscripten's own <GLES3/gl3.h>.
  target_compile_definitions(kfm_sokol PUBLIC SOKOL_GLES3)
else()
  # Desktop GL core loaded through glad (4.3 headers; macOS runs it on 4.1).
  target_compile_definitions(kfm_sokol PUBLIC SOKOL_GLCORE PRIVATE SOKOL_EXTERNAL_GL_LOADER)
  target_link_libraries(kfm_sokol PRIVATE kfm::glad)
endif()
target_link_libraries(kfm_sokol PUBLIC kfm::imgui)
set_target_properties(kfm_sokol PROPERTIES FOLDER third_party)
add_library(kfm::sokol ALIAS kfm_sokol)

# ---------------------------------------------------------------------------
# cgltf 1.15 (header only), for the skinned-model loader.
# ---------------------------------------------------------------------------
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  find_path(KFM_CGLTF_INCLUDE_DIR cgltf.h
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    NO_DEFAULT_PATH)
endif()
# cgltf ships no CMake package, so the pin is checked against the version
# line in the header's banner; any other release falls back to the fetch.
if(KFM_CGLTF_INCLUDE_DIR)
  file(STRINGS "${KFM_CGLTF_INCLUDE_DIR}/cgltf.h" kfm_cgltf_version_line
    REGEX "^ \\* Version: " LIMIT_COUNT 1)
  if(NOT kfm_cgltf_version_line MATCHES "Version: 1\\.15$")
    message(STATUS "KFM: ignoring cgltf in ${KFM_CGLTF_INCLUDE_DIR} ('${kfm_cgltf_version_line}', want 1.15)")
    unset(KFM_CGLTF_INCLUDE_DIR CACHE)
    unset(KFM_CGLTF_INCLUDE_DIR)
  endif()
endif()
add_library(kfm_cgltf INTERFACE)
if(KFM_CGLTF_INCLUDE_DIR)
  message(STATUS "KFM: cgltf from vcpkg (${KFM_CGLTF_INCLUDE_DIR})")
  target_include_directories(kfm_cgltf SYSTEM INTERFACE ${KFM_CGLTF_INCLUDE_DIR})
else()
  message(STATUS "KFM: cgltf not found via vcpkg, fetching v1.15")
  FetchContent_Declare(cgltf
    URL      https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  kfm-no-cmake)  # upstream CMakeLists adds tests; we only need the headers
  FetchContent_MakeAvailable(cgltf)
  target_include_directories(kfm_cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
endif()
add_library(kfm::cgltf ALIAS kfm_cgltf)
