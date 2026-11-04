#!/usr/bin/env bash
# Builds the C SDK example bots (tools/bots/examples/*.c) into the .wasm files
# checked in beside them (CI runners have no wasm32 clang; the example_bot
# test in kfm_ai_wasm_tests loads the checked-in chaser.wasm), and records the
# compiler in tools/bots/examples/BUILT_WITH.txt.
#   tools/bots/build_example.sh            # every example
#   tools/bots/build_example.sh chaser     # one
# The compiler: $CLANG, else clang on PATH when it targets wasm32 (with wasm-ld), else
# $EMSDK/upstream/bin/clang, else ~/emsdk/upstream/bin/clang.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dir="$here/examples"

# A usable compiler targets wasm32 and finds wasm-ld (beside it or on PATH).
targets_wasm32() {
  "$1" -print-targets 2>/dev/null | grep -q wasm32 &&
    command -v "$("$1" --target=wasm32 -print-prog-name=wasm-ld)" >/dev/null
}
cc="${CLANG:-}"
if [[ -z "$cc" ]]; then
  for c in "$(command -v clang || true)" "${EMSDK:-}/upstream/bin/clang" "$HOME/emsdk/upstream/bin/clang"; do
    if [[ -n "$c" && -x "$c" ]] && targets_wasm32 "$c"; then cc="$c"; break; fi
  done
fi
[[ -n "$cc" ]] || { echo "no clang with a wasm32 target (set CLANG or EMSDK)" >&2; exit 1; }

# No libc, no entry point, the four ABI exports and the shadow stack pointer
# (the host restores it after a trap). --allow-undefined turns the one
# declared import, env.kfm_log, into an import; the host refuses any other.
# -mcpu=mvp -mmutable-globals pins the wasm features (the MVP plus the exported
# mutable __stack_pointer) rather than the compiler's default set, so any wasm
# runtime loads the module; --no-wasm-opt keeps the clang driver from running
# binaryen's wasm-opt after the link (emsdk ships one; it would re-add
# features and make the output depend on what is installed).
flags=(--target=wasm32 -mcpu=mvp -mmutable-globals --no-wasm-opt -O2 -std=c11 -nostdlib
  -ffreestanding -Wall -Wextra -Werror
  -Wl,--no-entry -Wl,--export=kfm_abi_version -Wl,--export=kfm_obs_buffer
  -Wl,--export=kfm_init -Wl,--export=kfm_act -Wl,--export=__stack_pointer
  -Wl,--allow-undefined -Wl,-z,stack-size=65536 -Wl,--strip-all)

names=("$@")
if [[ ${#names[@]} -eq 0 ]]; then
  for c in "$dir"/*.c; do names+=("$(basename "${c%.c}")"); done
fi
for name in "${names[@]}"; do
  "$cc" "${flags[@]}" "$dir/$name.c" -o "$dir/$name.wasm"
  echo "$name.wasm: $(wc -c < "$dir/$name.wasm") bytes"
done
{
  echo "# The compiler that built the checked-in example .wasm files (tools/bots/build_example.sh)."
  "$cc" --version | head -n 1
} > "$dir/BUILT_WITH.txt"
