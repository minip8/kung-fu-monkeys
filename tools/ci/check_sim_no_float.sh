#!/usr/bin/env sh
# Determinism contract (architecture §5.1): no floating point inside engine/sim.
# Fails when engine/sim sources or headers contain a float/double type, a
# floating-point literal (1.5, 2.f, .5, 1e3), the Fixed float conversions
# (fromDouble, toFloat, toDouble) or <cmath>/<math.h>. Presentation converts
# Fixed at the boundary; core keeps those conversions for that purpose, which
# is why this check scopes to sim only. Line comments are stripped before the
# literal and conversion checks so prose such as "§5.2" does not trip them.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
types='\b(float|double|long double)\b'
literals='\b[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fFlL]?\b|(^|[^0-9A-Za-z_.])\.[0-9]+|\b[0-9]+[eE][+-]?[0-9]+[fFlL]?\b'
calls='\b(fromDouble|toFloat|toDouble)\b|#[[:space:]]*include[[:space:]]*<(cmath|math\.h)>'
status=0
report() {
  echo "error: floating point in engine/sim ($1):" >&2
  echo "$2" >&2
  status=1
}
if matches=$(grep -rnE "$types" "$root/engine/sim" --include='*.hpp' --include='*.cpp' --include='*.inc'); then
  report "float type" "$matches"
fi
files=$(find "$root/engine/sim" -name '*.hpp' -o -name '*.cpp' -o -name '*.inc' | sort)
for f in $files; do
  # Drop // comments (keeps #include lines, which carry no //), keep line numbers.
  if matches=$(sed -e 's://.*$::' "$f" | grep -nE "$literals|$calls"); then
    report "literal or conversion in $f" "$matches"
  fi
done
if [ "$status" -ne 0 ]; then exit 1; fi
echo "ok: no floating point in engine/sim"
