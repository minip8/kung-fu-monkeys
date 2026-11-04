# third_party

Code we vendor because no package manager provides it in a usable form. Everything else comes
from vcpkg or FetchContent (`cmake/KfmDependencies.cmake`).

- `glad/`: generated GL 4.3 core loader (see `glad/README.md`).
- `sokol/sokol_impl.cpp`: the single implementation TU for the fetched sokol headers
  (target `kfm::sokol`).
- `wamr/`: the build of the fetched WAMR 2.4.5 tarball as `kfm::wamr` (its configuration, a
  two-line source patch for instruction-budget read-back and metered instantiation, and
  `kfm_wamr_ext.{h,c}`, the accessors the patch feeds).

Nothing here is formatted with clang-format or checked by clang-tidy (both only run over
`engine apps tests tools`).
