// The cgltf implementation translation unit (kfm::cgltf is header only; the
// one includer that defines CGLTF_IMPLEMENTATION owns it). Compiled in its
// own small target without kfm::warnings: third-party code keeps its own
// warning level.
// NOLINTBEGIN
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4996)  // fopen and friends
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
// NOLINTEND
