#!/usr/bin/env bash
# Re-assembles the bot ABI test fixtures (tests/fixtures/bots/*.wat) into the
# .wasm files checked in beside them. CI runners have no wabt, so the .wasm
# files are committed; the bot_fixtures_up_to_date ctest
# (tools/bots/check_fixtures.cmake) fails when a .wasm no longer matches its
# .wat and is skipped where wat2wasm is not on PATH.
# Needs wabt's wat2wasm on PATH (verified with wabt 1.0.42).
set -euo pipefail
dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../tests/fixtures/bots" && pwd)"
command -v wat2wasm >/dev/null || { echo "wat2wasm (wabt) not found on PATH" >&2; exit 1; }
count=0
for wat in "$dir"/*.wat; do
  wat2wasm "$wat" -o "${wat%.wat}.wasm"
  count=$((count + 1))
done
echo "assembled ${count} fixtures in ${dir}"
