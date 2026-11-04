# kfm_wamr (kfm::wamr): WAMR 2.4.5, the wasm runtime behind WasmAgent
# (engine/ai, architecture §8), as one static C library. Added by
# cmake/KfmDependencies.cmake with KFM_WAMR_SOURCE_DIR set to the fetched
# release tarball. A directory of its own because WAMR's build scripts
# configure through directory-scoped include_directories(), add_definitions()
# and CMAKE_C_FLAGS; here they reach only kfm_wamr and kfm_wamr_ext.c.
#
# Configuration (every WAMR_BUILD_* is set before runtime_lib.cmake reads it):
#   - classic interpreter only (no fast interpreter, AOT, LLVM JIT or Fast
#     JIT): the only WAMR engine whose instruction metering is exact and the
#     same on every platform; no code generation, so W^X and Apple's JIT
#     entitlements never come up;
#   - instruction metering on (wasm_runtime_set_instruction_count_limit);
#   - no WASI, no libc-builtin, no threads, no shared memory, no debugger,
#     no multi-module: a bot imports nothing but env.kfm_log;
#   - software bounds checks (WAMR_DISABLE_HW_BOUND_CHECK): no signal
#     handlers, no 8 GiB guard reservation per instance (ten instances per
#     match), identical behaviour on Linux, macOS and Windows;
#   - SIMD off (the classic interpreter has none; WAMR's configure fails with
#     it on); bulk memory, reference types and extended constant expressions
#     on, because recent clang emits them for wasm32 by default (sign-ext,
#     non-trapping float-to-int, mutable globals and multi-value are always
#     on in WAMR);
#   - the C invokeNative (WAMR_BUILD_INVOKE_NATIVE_GENERAL): the host
#     registers its one native (kfm_log) in WAMR's "raw" form, which never
#     goes through invokeNative, so no per-architecture assembly is built;
#   - memory shrinking off: a module gets exactly the pages it declares.
#
# Source patch (applied in place at configure time, idempotent, fails loudly
# when the anchors move, i.e. when the pin changes):
#   1. core/iwasm/interpreter/wasm_interp_classic.c: the interpreter keeps
#      the remaining instruction budget in a local and never stores it back,
#      so a host cannot tell how much of the budget a call used. The patch
#      writes it to exec_env->instructions_to_execute at the loop's two exits
#      (return to the native caller, exception), where
#      kfm_wamr_instructions_left() reads it.
#   2. core/iwasm/common/wasm_exec_env.c: a new exec_env starts with
#      kfm_wamr_default_instruction_limit instead of -1 (unmetered), so the
#      start function and __wasm_call_ctors that wasm_runtime_instantiate
#      runs on its own internal exec_env are metered too.

if(NOT KFM_WAMR_SOURCE_DIR)
  message(FATAL_ERROR "third_party/wamr: KFM_WAMR_SOURCE_DIR is not set")
endif()

# --- source patch ------------------------------------------------------------
# One anchor per call: the anchors hold semicolons, which a CMake list would
# split. Skipped when the replacement is already there (a reconfigure).
function(kfm_wamr_patch relpath anchor replacement)
  set(path "${KFM_WAMR_SOURCE_DIR}/${relpath}")
  file(READ "${path}" text)
  string(FIND "${text}" "${replacement}" done)
  if(NOT done EQUAL -1)
    return()
  endif()
  string(FIND "${text}" "${anchor}" first)
  string(FIND "${text}" "${anchor}" final REVERSE)
  if(first EQUAL -1 OR NOT first EQUAL final)
    message(FATAL_ERROR "kfm_wamr: a patch anchor is not found exactly once in ${relpath} "
                        "(the WAMR pin changed?)")
  endif()
  string(REPLACE "${anchor}" "${replacement}" text "${text}")
  file(WRITE "${path}" "${text}")
endfunction()

set(kfm_write_back
"#if WASM_ENABLE_INSTRUCTION_METERING != 0 /* kfm patch: keep the budget left */
            if (exec_env)
                exec_env->instructions_to_execute = instructions_left;
#endif
")
kfm_wamr_patch(core/iwasm/interpreter/wasm_interp_classic.c
"        if (!prev_frame->ip) {
            /* Called from native. */
            return;"
"        if (!prev_frame->ip) {
            /* Called from native. */
${kfm_write_back}            return;")
kfm_wamr_patch(core/iwasm/interpreter/wasm_interp_classic.c
"        SYNC_ALL_TO_FRAME();
        return;

#if WASM_ENABLE_LABELS_AS_VALUES == 0
    }
#else
    FETCH_OPCODE_AND_DISPATCH();
#endif
}"
"        SYNC_ALL_TO_FRAME();
${kfm_write_back}        return;

#if WASM_ENABLE_LABELS_AS_VALUES == 0
    }
#else
    FETCH_OPCODE_AND_DISPATCH();
#endif
}")
kfm_wamr_patch(core/iwasm/common/wasm_exec_env.c
"WASMExecEnv *
wasm_exec_env_create_internal("
"/* kfm patch: third_party/wamr/kfm_wamr_ext.c */
extern int kfm_wamr_default_instruction_limit;

WASMExecEnv *
wasm_exec_env_create_internal(")
kfm_wamr_patch(core/iwasm/common/wasm_exec_env.c
"    exec_env->instructions_to_execute = -1;"
"    exec_env->instructions_to_execute = kfm_wamr_default_instruction_limit;")

# --- platform and target -----------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(WAMR_BUILD_PLATFORM "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(WAMR_BUILD_PLATFORM "darwin")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(WAMR_BUILD_PLATFORM "windows")
else()
  message(FATAL_ERROR "kfm_wamr: no WAMR platform for ${CMAKE_SYSTEM_NAME}; "
                      "configure with -DKFM_WASM_BOTS=OFF")
endif()

# One architecture per build: WAMR compiles BUILD_TARGET_* into every object,
# so a macOS universal build (two CMAKE_OSX_ARCHITECTURES) is not supported.
set(kfm_wamr_arch "${CMAKE_SYSTEM_PROCESSOR}")
list(LENGTH CMAKE_OSX_ARCHITECTURES kfm_osx_arch_count)
if(kfm_osx_arch_count EQUAL 1)
  set(kfm_wamr_arch "${CMAKE_OSX_ARCHITECTURES}")
elseif(kfm_osx_arch_count GREATER 1)
  message(FATAL_ERROR "kfm_wamr: universal builds are not supported; "
                      "set one CMAKE_OSX_ARCHITECTURES or -DKFM_WASM_BOTS=OFF")
endif()
string(TOLOWER "${kfm_wamr_arch}" kfm_wamr_arch)
if(kfm_wamr_arch MATCHES "^(x86_64|amd64|x64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WAMR_BUILD_TARGET "X86_64")
elseif(kfm_wamr_arch MATCHES "^(x86_64|amd64|x64|x86|i[3-6]86)$")
  set(WAMR_BUILD_TARGET "X86_32")
elseif(kfm_wamr_arch MATCHES "^(aarch64|arm64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WAMR_BUILD_TARGET "AARCH64")
elseif(kfm_wamr_arch MATCHES "^arm")
  set(WAMR_BUILD_TARGET "ARM")
else()
  message(FATAL_ERROR "kfm_wamr: no WAMR target for processor '${kfm_wamr_arch}'; "
                      "configure with -DKFM_WASM_BOTS=OFF")
endif()

# --- features ----------------------------------------------------------------
set(WAMR_ROOT_DIR "${KFM_WAMR_SOURCE_DIR}")
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 0)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LAZY_JIT 0)
set(WAMR_BUILD_INSTRUCTION_METERING 1)
set(WAMR_BUILD_SIMD 0)
set(WAMR_BUILD_BULK_MEMORY 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_EXTENDED_CONST_EXPR 1)
set(WAMR_BUILD_TAIL_CALL 0)
set(WAMR_BUILD_GC 0)
set(WAMR_BUILD_STRINGREF 0)
set(WAMR_BUILD_EXCE_HANDLING 0)
set(WAMR_BUILD_MEMORY64 0)
set(WAMR_BUILD_MULTI_MEMORY 0)
set(WAMR_BUILD_SHARED_MEMORY 0)
set(WAMR_BUILD_SHRUNK_MEMORY 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_BUILD_MINI_LOADER 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_LIBC_UVWASI 0)
set(WAMR_BUILD_LIBC_EMCC 0)
set(WAMR_BUILD_LIB_PTHREAD 0)
set(WAMR_BUILD_LIB_PTHREAD_SEMAPHORE 0)
set(WAMR_BUILD_LIB_WASI_THREADS 0)
set(WAMR_BUILD_THREAD_MGR 0)
set(WAMR_BUILD_WASI_NN 0)
set(WAMR_BUILD_LIB_RATS 0)
set(WAMR_BUILD_SHARED_HEAP 0)
set(WAMR_BUILD_DEBUG_INTERP 0)
set(WAMR_BUILD_DEBUG_AOT 0)
set(WAMR_BUILD_SPEC_TEST 0)
set(WAMR_BUILD_WASI_TEST 0)
set(WAMR_BUILD_CUSTOM_NAME_SECTION 0)
set(WAMR_BUILD_LOAD_CUSTOM_SECTION 0)
set(WAMR_BUILD_DUMP_CALL_STACK 0)
set(WAMR_BUILD_PERF_PROFILING 0)
set(WAMR_BUILD_MEMORY_PROFILING 0)
set(WAMR_BUILD_LINUX_PERF 0)
set(WAMR_BUILD_WASM_CACHE 0)
set(WAMR_BUILD_QUICK_AOT_ENTRY 0)
set(WAMR_BUILD_INVOKE_NATIVE_GENERAL 1)
set(WAMR_BUILD_SANITIZER "")       # never from the environment; KFM_SANITIZE applies instead
set(WAMR_DISABLE_HW_BOUND_CHECK 1)
set(WAMR_DISABLE_STACK_HW_BOUND_CHECK 1)
set(WAMR_DISABLE_WAKEUP_BLOCKING_OP 1)
set(WAMR_DISABLE_WRITE_GS_BASE 1)  # AOT only; also skips WAMR's try_run probe
set(WAMR_DISABLE_APP_ENTRY 1)      # no wasm_application.c (main/argv entry helpers)

# WAMR prints its whole configuration with message(); keep the configure quiet.
set(CMAKE_MESSAGE_LOG_LEVEL WARNING)
include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
unset(CMAKE_MESSAGE_LOG_LEVEL)

add_library(kfm_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE} kfm_wamr_ext.c)
add_library(kfm::wamr ALIAS kfm_wamr)
target_include_directories(kfm_wamr SYSTEM INTERFACE
  ${WAMR_ROOT_DIR}/core/iwasm/include
  ${CMAKE_CURRENT_SOURCE_DIR})
# A static library: its users see the API undecorated (wasm_export.h would
# otherwise declare it dllimport on MSVC, which does not link against a
# static library). INTERFACE, never PUBLIC: WAMR's own translation units
# must keep the dllexport that COMPILING_WASM_RUNTIME_API gives them, because
# platform_common.h declares wasm_runtime_malloc/free with an explicit
# __declspec(dllexport) and MSVC rejects a second declaration with another
# linkage (C2375). A user includes wasm_export.h alone, so it is consistent.
target_compile_definitions(kfm_wamr INTERFACE "WASM_RUNTIME_API_EXTERN=")
# WAMR ships a .clang-tidy whose keys clang-tidy 19 (the CI pin) rejects
# (AnalyzeTemporaryDtors), and clang-tidy reads the nearest config for every
# file a diagnostic lands in, headers included. Third-party code is never
# tidied, so the fetched tree gets a config that says so.
file(WRITE "${WAMR_ROOT_DIR}/.clang-tidy"
  "# Written by third_party/wamr/CMakeLists.txt: third-party code is never tidied.\nChecks: '-*'\n")
if(MSVC)
  target_compile_options(kfm_wamr PRIVATE /w)
else()
  # Third-party code: its own flags (runtime_lib.cmake adds -Wall), and no
  # warnings in our build logs.
  target_compile_options(kfm_wamr PRIVATE -w)
  if(KFM_SANITIZE MATCHES "undefined")
    # The interpreter reads operands with unaligned loads on purpose
    # (WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS); WAMR's own UBSan
    # configuration excludes the same check.
    target_compile_options(kfm_wamr PRIVATE -fno-sanitize=alignment)
  endif()
  if(KFM_SANITIZE MATCHES "address")
    # WAMR guards the native stack by comparing a local's address with the
    # thread's stack bounds; ASan's use-after-return fake stacks put that
    # local on the heap and every call fails with "native stack overflow".
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      target_compile_options(kfm_wamr PRIVATE -fsanitize-address-use-after-return=never)
    else()
      target_compile_options(kfm_wamr PRIVATE --param=asan-use-after-return=0)
    endif()
  endif()
endif()
if(WIN32)
  target_link_libraries(kfm_wamr PUBLIC ntdll ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kfm_wamr PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  if(NOT APPLE)
    target_link_libraries(kfm_wamr PUBLIC m)
  endif()
endif()
set_target_properties(kfm_wamr PROPERTIES FOLDER third_party)
