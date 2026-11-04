// Observation v1 (architecture §8): the versioned, little-endian, fixed-layout
// POD every bot reads. PLACEHOLDER: the v1 field list lands with the
// observation builder; only the version constant and the type name are
// pinned here so WasmAgent can compile against sizeof(Observation).
#pragma once

#include <cstdint>
#include <type_traits>

namespace kfm::ai {

inline constexpr uint32_t kObservationVersion = 1;

struct Observation {
  uint32_t version{kObservationVersion};
};
static_assert(std::is_trivially_copyable_v<Observation>);

}  // namespace kfm::ai
