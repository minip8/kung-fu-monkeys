#include "kfm/core/assert.hpp"

#include <cstdio>
#include <cstdlib>

namespace kfm::detail {

[[noreturn]] void assertFail(const char* expr, const char* file, int line, const char* msg) {
  if (msg != nullptr) {
    std::fprintf(stderr, "KFM assertion failed: %s (%s) at %s:%d\n", expr, msg, file, line);
  } else {
    std::fprintf(stderr, "KFM assertion failed: %s at %s:%d\n", expr, file, line);
  }
  std::fflush(stderr);
  std::abort();
}

}  // namespace kfm::detail
