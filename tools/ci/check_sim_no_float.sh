#!/usr/bin/env sh
# Determinism contract (architecture §5.1): no floating point inside engine/sim.
# Fails when any float/double token appears in engine/sim sources or headers.
# Presentation converts Fixed at the boundary; core keeps toFloat()/toDouble()
# for that purpose, which is why this check scopes to sim only.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
if matches=$(grep -rnE '\b(float|double|long double)\b' "$root/engine/sim" --include='*.hpp' --include='*.cpp' --include='*.inc'); then
  echo "error: floating point in engine/sim:" >&2
  echo "$matches" >&2
  exit 1
fi
echo "ok: no floating point in engine/sim"
