// Assertions and unreachable markers for all layers, including the
// deterministic sim: no exceptions, no RTTI, no allocation. A failed
// assertion prints and aborts.
#pragma once

#include <cstdlib>

namespace kfm::detail {
[[noreturn]] void assertFail(const char* expr, const char* file, int line, const char* msg);
}  // namespace kfm::detail

#if defined(NDEBUG) && !defined(KFM_ASSERTS_IN_RELEASE)
#define KFM_ASSERTS_ENABLED 0
#else
#define KFM_ASSERTS_ENABLED 1
#endif

#if KFM_ASSERTS_ENABLED
#define KFM_ASSERT(cond) \
  ((cond) ? static_cast<void>(0) : ::kfm::detail::assertFail(#cond, __FILE__, __LINE__, nullptr))
#define KFM_ASSERT_MSG(cond, msg) \
  ((cond) ? static_cast<void>(0) : ::kfm::detail::assertFail(#cond, __FILE__, __LINE__, (msg)))
#else
#define KFM_ASSERT(cond) static_cast<void>(sizeof(cond))
#define KFM_ASSERT_MSG(cond, msg) static_cast<void>(sizeof(cond))
#endif

// Always-on check for conditions that must hold even in release builds
// (decoder bounds, pool invariants). Cheap enough to keep.
#define KFM_CHECK(cond) \
  ((cond) ? static_cast<void>(0) : ::kfm::detail::assertFail(#cond, __FILE__, __LINE__, nullptr))

// A static_assert over a table of function pointers (every hook of every
// behaviour row is set). GCC cannot fold "an inline function's address is
// not null" under -fsanitize: the sanitizers imply
// -fno-delete-null-pointer-checks, which makes a weak symbol's address a
// possible null. KFM_SANITIZE_BUILD comes from cmake/KfmSanitizers.cmake;
// every other build keeps the compile-time check, so the sanitizer build
// only checks that the condition is well-formed.
#ifdef KFM_SANITIZE_BUILD
#define KFM_STATIC_ASSERT_HOOKS(cond, msg) static_assert(sizeof(cond) > 0, msg)
#else
#define KFM_STATIC_ASSERT_HOOKS(cond, msg) static_assert(cond, msg)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define KFM_UNREACHABLE() __assume(0)
#else
#define KFM_UNREACHABLE() __builtin_unreachable()
#endif
