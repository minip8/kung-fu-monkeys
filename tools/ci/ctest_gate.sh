#!/usr/bin/env bash
# A CI leg's test run (architecture §9, v0.85):
#
#   tools/ci/ctest_gate.sh <test preset> [label regex the leg leaves out]
#
# Everything but the real-socket tests (label `network`) runs once, four at a
# time: a failure there is a failure. The real-socket tests then run on their
# own and a failing one is run again, up to three attempts in all: they open
# UDP sockets on 127.0.0.1 and go through a real DTLS handshake, which fails
# now and then on a shared runner (and about once in eighty runs on a quiet
# machine) in a different test each time. A test that needed another attempt
# leaves a warning on the run, so the flake rate stays visible; a transport
# that is broken fails all three and fails the leg.
#
# Nothing else is ever retried. A second attempt at a determinism or sim test
# would hide exactly the nondeterminism those tests exist to catch.
set -euo pipefail

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
  echo "usage: $0 <test preset> [label regex to leave out]" >&2
  exit 2
fi
preset=$1
skip=${2:-}
attempts=3

# Both parts always run, so one run shows every failure.
first=0
ctest --preset "$preset" --parallel 4 -LE "${skip:+$skip|}network" || first=$?

log=$(mktemp)
trap 'rm -f "$log"' EXIT
status=0
ctest --preset "$preset" -L network --no-tests=ignore --repeat "until-pass:$attempts" 2>&1 |
  tee "$log" || status=$?
# ctest prints one result line per attempt ("3/5 Test #24: name ...***Failed",
# without the "3/5" from the second attempt on); a line that is not "Passed"
# in a run that passed is an attempt that was repeated.
flaked=$(grep -E '^ *([0-9]+/[0-9]+ )?Test +#[0-9]+: .*\*\*\*' "$log" |
  sed -E 's/^ *([0-9]+\/[0-9]+ )?Test +#[0-9]+: ([A-Za-z0-9_]+).*/\2/' | sort -u | tr '\n' ' ' || true)
if [ -n "$flaked" ]; then
  if [ "$status" -eq 0 ]; then
    echo "::warning title=Real-socket test needed another attempt::${flaked}passed within $attempts attempts ($preset)"
  else
    echo "::error title=Real-socket tests failed::not every test passed within $attempts attempts; with a failed attempt: ${flaked}($preset)"
  fi
fi
if [ "$first" -ne 0 ]; then
  exit "$first"
fi
exit "$status"
