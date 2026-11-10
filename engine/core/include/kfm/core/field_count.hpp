// fieldCount<T>(): the number of direct members of an aggregate, at compile
// time. visit.hpp asserts it next to every visit() overload, so a field
// added to a serialised struct and forgotten in its visit() is a compile
// error rather than state that silently escapes the checksum, snapshots and
// the content hash (architecture §5.2).
//
// The technique is the aggregate-arity probe Boost.PFR's fields_count uses:
// T{a0, ..., ak-1} with k placeholders that convert to any type is
// well-formed exactly for k <= the member count (fewer initialisers leave
// the rest to their default member initialisers). Because a placeholder can
// initialise any member directly, aggregate initialisation never elides
// braces into a member: a std::array or a nested struct counts as one field.
// Limits, each a compile error, never a wrong count that passes:
//   - T must be an aggregate (static_assert below); a constructor would make
//     the probe count constructor arguments.
//   - A C array member (int a[4]) cannot be the target of a conversion
//     function, so brace elision would count its elements: there are none in
//     the serialised structs, use std::array.
//   - A member type with converting constructors of its own (std::optional,
//     say) makes its placeholder ambiguous and the count stops short of it.
//   - Base classes count as leading members; the serialised structs have none.
// The probe is classic SFINAE in decltype (no requires-expression over a
// pack), the form PFR has shipped on GCC, Clang and MSVC for years.
#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace kfm {
namespace detail {

// A placeholder for the I-th initialiser; converts to anything. Declared,
// never defined: it is only ever named in unevaluated operands.
template <std::size_t I>
struct AnyField {
  template <class T>
  // NOLINTNEXTLINE(misc-explicit-constructor): converting to anything is the point
  constexpr operator T() const noexcept;
};

template <class T, std::size_t... I>
constexpr auto braceInitializable(std::index_sequence<I...> /*k*/, int /*preferred*/)
    -> decltype(static_cast<void>(T{AnyField<I>{}...}), true) {
  return true;
}
template <class T, std::size_t... I>
constexpr bool braceInitializable(std::index_sequence<I...> /*k*/, long /*fallback*/) {
  return false;
}

inline constexpr std::size_t kMaxProbedFields = 64;

// Probes downwards from kMaxProbedFields and stops at the first k that is
// well-formed, which is the member count itself: no probe ever leaves a
// member without an initialiser, so -Wmissing-field-initializers (Clang
// warns about one even in an unevaluated operand) never fires.
template <class T, std::size_t N>
constexpr std::size_t countFieldsDownFrom() {
  if constexpr (N == 0) {
    return 0;
  } else if constexpr (braceInitializable<T>(std::make_index_sequence<N>{}, 0)) {
    return N;
  } else {
    return countFieldsDownFrom<T, N - 1>();
  }
}

}  // namespace detail

template <class T>
[[nodiscard]] consteval std::size_t fieldCount() {
  static_assert(std::is_aggregate_v<T>, "fieldCount needs an aggregate");
  constexpr std::size_t kCount = detail::countFieldsDownFrom<T, detail::kMaxProbedFields>();
  static_assert(kCount < detail::kMaxProbedFields, "more fields than fieldCount probes");
  return kCount;
}

}  // namespace kfm
