#!/usr/bin/env sh
# Determinism contract (architecture §5.1, §8): no floating point inside
# engine/sim or engine/ai. The sim is fixed-point by contract; engine/ai (the
# observation builder, the reference bot, the agent host) is too, because a
# bot match must reproduce bit for bit on every machine and in the wasm
# build. Fails when a source or header under either tree contains a
# float/double type, a floating-point literal (1.5, 2.f, .5, 1e3), the Fixed
# float conversions (fromDouble, toFloat, toDouble) or <cmath>/<math.h>.
# Presentation converts Fixed at the boundary; core keeps those conversions
# for that purpose, which is why this check scopes to these two trees only.
# String literals and line comments are stripped before the literal and
# conversion checks so prose such as "§5.2" does not trip them.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
dirs="engine/sim engine/ai"
types='\b(float|double|long double)\b'
literals='\b[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fFlL]?\b|(^|[^0-9A-Za-z_.])\.[0-9]+|\b[0-9]+[eE][+-]?[0-9]+[fFlL]?\b'
calls='\b(fromDouble|toFloat|toDouble)\b|#[[:space:]]*include[[:space:]]*<(cmath|math\.h)>'
status=0
report() {
  echo "error: floating point in $1 ($2):" >&2
  echo "$3" >&2
  status=1
}
for dir in $dirs; do
  if matches=$(grep -rnE "$types" "$root/$dir" --include='*.hpp' --include='*.cpp' --include='*.inc'); then
    report "$dir" "float type" "$matches"
  fi
  files=$(find "$root/$dir" -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' | sort)
  for f in $files; do
    # Blank string literals, then drop // comments; line numbers are kept.
    if matches=$(sed -E -e 's/"([^"\\]|\\.)*"/""/g' -e 's://.*$::' "$f" | grep -nE "$literals|$calls"); then
      report "$dir" "literal or conversion in $f" "$matches"
    fi
  done
done
if [ "$status" -ne 0 ]; then exit 1; fi
echo "ok: no floating point in engine/sim or engine/ai"
