# tools/bots

Bot tooling kept in the game repository (architecture §8). The normative contract is
`docs/bot-abi.md` (bot ABI and `Observation` v2; the host still runs a module built against
version 1, architecture v0.80).

| Path | What |
|---|---|
| `sdk-c/kfm_bot.h` | The seed of the C SDK: one freestanding C11 header (no libc). |
| `examples/chaser.c`, `examples/idle.c`, `examples/gatherer.c` | Example bots built from the header, with the `.wasm` built from each checked in beside it. `gatherer` (architecture v0.82) is the harvest kind's smoke test: it carries bananas from the trees to its home zone, each monkey working the tree its rank in the team gives it, and throws at an enemy in a clear line when it holds two or more. |
| `examples/BUILT_WITH.txt` | The compiler that built the checked-in `.wasm` files. |
| `build_example.sh` | Builds the examples (`tools/bots/build_example.sh [name...]`). |
| `build_fixtures.sh`, `check_fixtures.cmake` | The hand-written ABI test modules in `tests/fixtures/bots` (`.wat` to `.wasm`, wabt). |

**The SDK's home is the separate bots repository**,
[`minip8/kung-fu-monkeys-bots`](https://github.com/minip8/kung-fu-monkeys-bots) (M7 step 2):
the C header copied byte for byte (its CI diffs the copy against this file at the engine commit
its `engine.lock` names), the C++ layer, the example bots, the build recipe and image, and
later the ladder. What is here is the seed copied there, and the in-repo proof that a module
built from it loads and plays: `tests/unit/ai/example_bot_test.cpp` (in `kfm_ai_wasm_tests`)
checks the header's layout and constants against `engine/ai/include/kfm/ai/observation.hpp`,
its integer math and banana flight step against the engine and the sim, plays the checked-in
`chaser.wasm` through `AgentHost` against the reference bot, and plays `gatherer.wasm` through
harvest matches on a hand-built arena with walls, pits, trees and two homes (in the grid, in the
continuous space and under fog, against itself and against the reference bot), each to its
`score_to_win`. A change to the header or to an
example is followed by `tools/bots/build_example.sh` and a commit of the rebuilt `.wasm`.
`tests/fixtures/bots/chaser_v1.wasm` is the chaser as built against the version 1 header (the
`chaser.wasm` checked in before v0.80, byte for byte); it is never rebuilt, and the same test
plays it against the version 2 build to show a version 1 module runs unchanged.

## The header

Copy `kfm_bot.h` into a project, define `KFM_BOT_MAIN` in one `.c` file before including it
(that defines `kfm_abi_version`, `kfm_obs_buffer`, the observation buffer `kfm_obs` and the
`memcpy`/`memmove`/`memset` the compiler may call), and write `kfm_init` and `kfm_act`:

```c
#define KFM_BOT_MAIN
#include "kfm_bot.h"

void kfm_init(uint64_t seed, uint32_t slot) { /* seed a kfm_rng, reset state */ }
uint64_t kfm_act(uint32_t tick) {
  const kfm_observation* o = &kfm_obs;  /* written by the host before each call */
  return kfm_pack_input(/*moveDir*/ 0, /*moveMag*/ 0, /*aimDir*/ o->self.facing, /*buttons*/ 0, 0);
}
```

Besides the ABI (the `Observation` v2 structs with `_Static_assert`ed layout, the enum and
button constants, `kfm_pack_input`, the export and `kfm_log` import declarations) it has
integer helpers that behave identically on every host: Q16.16 fixed point (`kfm_fx_*`,
`kfm_vec_*`), the engine's sine table and atan2 (`kfm_sin`, `kfm_cos`, `kfm_angle_of`,
`kfm_angle8_of`, `kfm_dir_to`, `kfm_angle_to_vec`), the engine's xoshiro128** (`kfm_rng_*`),
a log text builder (`kfm_text_*`), and game helpers (`kfm_nearest_enemy`,
`kfm_nearest_threat`, `kfm_ability_ready`, `kfm_time_to_impact`, `kfm_segment_min_sq`, the
banana flight step and predictor `kfm_flight_step` / `kfm_predict_banana`, and
`kfm_dash_catch_dir`). The header comment says which are exact against the sim.

## Building a module

```sh
clang --target=wasm32 -mcpu=mvp -mmutable-globals --no-wasm-opt -O2 -std=c11 \
  -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export=kfm_abi_version -Wl,--export=kfm_obs_buffer \
  -Wl,--export=kfm_init -Wl,--export=kfm_act -Wl,--export=__stack_pointer \
  -Wl,--allow-undefined -Wl,-z,stack-size=65536 -Wl,--strip-all bot.c -o bot.wasm
```

Any clang with the WebAssembly target and `wasm-ld` works; `build_example.sh` uses `$CLANG`,
else `clang` on `PATH`, else emsdk's (`$EMSDK/upstream/bin/clang`, `~/emsdk/upstream/bin/clang`).
`-mcpu=mvp -mmutable-globals` keeps the module to the MVP feature set (plus the exported mutable
global) whatever the compiler's default; `--no-wasm-opt` stops the clang driver from running a
`wasm-opt` it finds (emsdk ships one), which would re-enable features and make the output depend
on what is installed.

A module must:

- export `kfm_abi_version () -> i32` (returns 2, the header's `KFM_ABI_VERSION`; a module built
  against version 1 returns 1 and still runs), `kfm_obs_buffer () -> i32` (a 4-aligned address
  of at least 5880 bytes, 5748 for version 1, the same for the life of the instance),
  `kfm_init (i64 seed, i32 slot)` and `kfm_act (i32 tick) -> i64` (the packed input in the low
  40 bits, bits 40..63 zero);
- export its linear memory (wasm-ld does by default) and its shadow stack pointer
  `__stack_pointer` (`--export=__stack_pointer`): the host restores it after a trap or an
  over-budget tick, which would otherwise leak a stack frame per fault;
- import nothing but, optionally, `env.kfm_log (i32 ptr, i32 len)`. An unresolved libc call
  (`memcpy` from a struct copy, say) becomes an import and the host refuses the module;
  `KFM_BOT_MAIN` provides the three the compiler emits.

Budgets (host defaults, `docs/bot-abi.md` §1.4): 100 000 wasm instructions per `kfm_act`,
10 000 000 for instantiation and for `kfm_init`, a 4 MiB file and 16 MiB of memory. The chaser
uses about 7 000 instructions per act on average and under 30 000 at most. The gatherer uses about
3 000 to 4 000 on average and under 35 000 at most on the contest's maps (28 x 16 cells at the
largest): it builds its cell map and its distance field to the home over ticks of their own in
the round's countdown (the map, the steps in two halves, the wall cells, then the field 128
cells a tick), and grows the field to the tree it works or the banana it fetches by at most 96
cells a tick, walking straight at the goal until the field reaches it.

The tree a gatherer works is a share-out every monkey of a team computes alike, from the
statics and the roster, with no call: the trees are weighted by what a trip to one costs, its
beat plus twice its walking distance from the home, and the team's r-th monkey by slot takes the
r-th pick of a highest-averages (D'Hondt) share-out, so the rich and near trees get more
monkeys. Five a side on the contest's pool, a team sends three monkeys to the two rich trees and
one to each flank tree (on `orchards`, with no rich tree, one to each of the four flank trees
and one to the home tree), so the two teams meet at the best trees.

The smoke run on the `contest` row (`kfm_arena --mode contest --arena NAME --team G,G,G,G,G
--team G,G,G,G,G` or `--team ref:hard,...`, `--matches 20 --rotate --seed 1`; `own` fog, 40 banked
to win, a 3600-turn clock), architecture v0.82: no fault in any of the 160 games, no game at the
clock.

| Arena | Pairing | fuel_max | Draws | Median ticks | Mean score (side 0 - 1, or gatherer - ref) | Kills (gatherer / other) | Wins side 0 / 1 |
|---|---|---:|---:|---:|---|---|---|
| `grove` | mirror | 33 668 | 0 | 490 | 40 - 12 | 0 / 0 | 20 / 0 |
| `grove` | vs `ref:hard` | 33 668 | 0 | 971 | 40.1 - 3.1 | 128 / 374 | 10 / 10 |
| `orchards` | mirror | 34 411 | 20 | 874 | 40 - 40 | 0 / 0 | 0 / 0 |
| `orchards` | vs `ref:hard` | 34 411 | 0 | 853 | 40.1 - 1.8 | 38 / 210 | 10 / 10 |
| `gorge` | mirror | 33 745 | 20 | 788 | 40 - 40 | 0 / 0 | 0 / 0 |
| `gorge` | vs `ref:hard` | 33 745 | 0 | 1146 | 40.1 - 1.2 | 97 / 405 | 10 / 10 |
| `burrows` | mirror | 27 500 | 20 | 672 | 40 - 40 | 0 / 0 | 0 / 0 |
| `burrows` | vs `ref:hard` | 27 500 | 0 | 794 | 40.1 - 5.2 | 19 / 156 | 10 / 10 |

The gatherer ignores its seed, so a mirror of gatherers is one game twenty times: the mirror
maps' half turn and the tie-free share-out give both sides the same bananas on the same turn and
a 40-40 draw, but on `grove` both teams' first picks are the same rich tree (equal costs, the
lower spawner) and, standing on its cell together, side 0's lower slots take its fruit. Against
`ref:hard` the gatherers win all 80 games; the side split there is `--rotate`'s.
