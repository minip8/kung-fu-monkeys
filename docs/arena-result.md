# Kung Fu Monkeys — `kfm_arena` JSON result

Status: normative, append-only since architecture v0.36, pinned by a test since v0.60
(2026-11-07). The design and its rationale are in `docs/architecture.md` §8 "The ladder" and §11
"M7 sequencing": the bot ladder (another repository) runs `kfm_arena --json`, parses the objects
below and records the release tag per match, across releases. The C++ source of truth is
`formatResultJson` and `formatSummaryJson` in `apps/arena/arena_core.cpp`; a unit test
(`tests/unit/arena/arena_test.cpp`, "the JSON result's keys are the frozen contract") checks
every object's keys, their order and their JSON types against frozen lists that mirror the
tables below. Where this document and that test disagree, one of them has a bug: fix it before
release.

## 1. The stream

`kfm_arena --json` prints one JSON object per line on stdout, each line a complete object
(UTF-8, no whitespace between tokens, no newline inside, terminated by `\n`):

- one **match object** (§2) per match, in match order, as each match ends;
- one **summary object** (§3) last, after every match.

The summary object is the only one with the key `"summary"` (always `true`); a reader tells the
two apart by its presence. `--quiet` drops the match objects from stdout (the summary still
prints). `--result FILE` writes the same lines, match objects included, to FILE. Diagnostics go
to stderr, never stdout; so does `--log`'s module output, after the summary.

The text lines `kfm_arena` prints without `--json` (`formatResultLine`, `formatSummaryLine`) are
for humans and make no promise: their fields may change in any release.

Conventions:

- *number*: a JSON integer, except `mean_place` (a decimal with two digits after the point).
  Counters are unsigned; `score` is signed (a suicide may take it below 0, v0.57).
- *hash*: a string of exactly 8 lowercase hex digits, a u32 zero-padded (`"0c0ffee1"`).
- *spec*: a bot as `BotSpec` text: `ref:easy`, `ref:medium`, `ref:hard` or the module's path as
  given on the command line (JSON-escaped).
- *slot*: a player slot, 0..9. Slots are numbered from 0 with no gaps.
- "Present" says when a key appears. A key that is not present is absent, never `null`; a key
  marked *or null* is always present and may be `null`.

## 2. The match object

| Key | Type | Present | Meaning |
|---|---|---|---|
| `match` | number | always | The match index k, 0-based (`--matches M`: 0..M-1). |
| `seed` | number | always | The world seed (u64): `--seed` + k. Slot i's bot is seeded with `ai::agentSeed(seed, i)` (`engine/ai/include/kfm/ai/agent_host.hpp`), every host's formula. |
| `ticks` | number | always | Sim ticks run, the replay's `tickCount`. |
| `checksum` | hash | always | `sim::checksum` of the final state: what replaying the match's `.kfmr` reaches. |
| `result` | string | always | `"ended"` (the sim's `MatchEnded`) or `"cap"` (the `--ticks` cap came first). |
| `winner` | number or null | always | The winning slot; `null` for a draw (`ended` without a winner) and always for `cap`. |
| `winner_team` | number or null | team mode | The winning slot's team (the mode's team index, its side); `null` when `winner` is. |
| `teams` | array of team (§2.1) | team mode | One entry per team, by team index. |
| `slots` | array of slot (§2.2) | always | One entry per slot, by slot. |
| `content_hash` | hash | always (v0.60) | `sim::contentHash` of the match's content: the replay header's `contentHash`. |
| `build_hash` | hash | always (v0.60) | The replay header's `buildHash`: the build id of the `kfm_arena` that ran the match (architecture §5.1). `"00000000"` for an unstamped development build; since v0.70 a release's binaries carry the first 8 hex digits of the tagged commit (never zero), the same id as that release's server and page. |
| `arena` | string | always (v0.81) | The name of the arena the match played (the content's `[[arena]] name`). Without `--arena`, a mode with an arena pool (`arenas`) plays match k on pool member (k / T) mod P in content order (P its members that suit the mode, T the team count with `--team`, else the slot count), so with `--rotate` every entrant plays every side of a member before the next; `--arena NAME`, or a mode without a pool, plays one arena every match. |

A team mode is one run with `--team` (a mode with two or more teams); without it `winner_team`,
`teams` and the slots' `team` are absent.

### 2.1 A `teams[]` entry

| Key | Type | Present | Meaning |
|---|---|---|---|
| `team` | number | always | The mode's team index (its side). |
| `entrant` | number | always | Which `--team` played it, 0-based in command-line order (`--rotate`: entrant e plays team (e + k) mod T). |
| `spec` | string | always | The specs of its slots in slot order, comma-separated. |
| `place` | number | always | 1..T: the winner's team first, then the higher team score, fewer deaths, the lower team index. |
| `score` | number | always | The mode's team score (`MatchContent::teamScore`). |
| `kills` | number | always | Eliminations of enemies by its slots. |
| `deaths` | number | always | Its slots' deaths. |

### 2.2 A `slots[]` entry

| Key | Type | Present | Meaning |
|---|---|---|---|
| `slot` | number | always | The slot. |
| `bot` | number | always | The bot's index in the `--bot` order (the `--bots` padding last); with `--team`, entrant × team size + its position in the team. Equals `slot` without `--rotate` in a free-for-all. |
| `spec` | string | always | The bot's spec. |
| `module` | bool | always | Whether a `.wasm` module played the slot. |
| `team` | number | team mode | The slot's team. |
| `place` | number | always | 1..n, distinct: the winner first, then the higher score, fewer deaths, the lower slot. |
| `score` | number | always | The slot's final score (round wins or points, `MatchRules::scores`). |
| `kills` | number | always | Eliminations with this slot as the killer (never itself). |
| `deaths` | number | always | Eliminations of this slot. |
| `acts` | number | always | Ticks the bot was asked for an input (a monkey on the field). |
| `subs` | number | always | Ticks its input was substituted by "repeat last input" (a fault, over budget). |
| `traps` | number | module | Ticks `kfm_act` trapped. |
| `over` | number | module | Ticks `kfm_act` ran over its budget (`--fuel`). |
| `bad` | number | module | Ticks `kfm_act` returned a value with bits 40..63 set. |
| `fuel_mean` | number | module | Wasm instructions per act, the mean rounded down. |
| `fuel_max` | number | module | The most instructions one act used. |
| `fuel_init` | number | module | Instructions the latest `kfm_init` used. |
| `log` | number | module | Bytes the module passed to `kfm_log`. |
| `dead` | string or null | module | `null` while the module lives; else why it died: `trap`, `over-budget`, `bad-return`, `dead`, `instantiate`, `abi-mismatch`, `bad-obs-buffer`, `init-trap` or `init-over-budget` (`toString(AgentFault)`; new causes may be added). |

"module" means the slot's `module` is `true`; the module keys are absent for a reference bot.
The budgets and faults are `docs/bot-abi.md` §1.4.

## 3. The summary object

| Key | Type | Present | Meaning |
|---|---|---|---|
| `summary` | bool | always | Always `true`: marks the summary. |
| `matches` | number | always | Matches played. |
| `ended` | number | always | Matches that ended with a winner. |
| `draws` | number | always | Matches that ended without one. |
| `cap` | number | always | Matches the tick cap ended. |
| `ticks_mean` | number | always | Ticks per match, the mean rounded down (0 for no match). |
| `slots` | array of totals (§3.1) | always | Totals per slot, by slot. |
| `bots` | array of totals (§3.1) | rotated | Totals per bot, by bot index. Present when some match put a bot in a slot other than its index (`--rotate` over more than one match, or `--team`'s mapping). |
| `entrants` | array of entrant (§3.2) | team mode | Totals per entrant, by entrant index. |

### 3.1 A `slots[]` or `bots[]` totals entry

| Key | Type | Present | Meaning |
|---|---|---|---|
| `slot` / `bot` | number | always | The slot (in `slots`) or the bot index (in `bots`); the entry's first key. |
| `spec` | string | always | The spec; a slot several bots played is `"mixed"`. |
| `module` | bool | always | Whether a module played it in some match. |
| `wins` | number | always | Matches it was the winner of. |
| `mean_place` | number | always | The mean place over every match, two decimals. |
| `score` | number | always | The summed score. |
| `kills` | number | always | The summed kills. |
| `deaths` | number | always | The summed deaths. |
| `acts` | number | always | The summed acts. |
| `subs` | number | always | The summed substitutions. |
| `traps` | number | module | The summed traps. |
| `over` | number | module | The summed over-budget ticks. |
| `bad` | number | module | The summed bad returns. |
| `fuel_max` | number | module | The most instructions one act used, over every match. |

### 3.2 An `entrants[]` entry

| Key | Type | Present | Meaning |
|---|---|---|---|
| `entrant` | number | always | The entrant's index, in `--team` order. |
| `spec` | string | always | Its team's specs, as in §2.1. |
| `wins` | number | always | Matches its team won. |
| `mean_place` | number | always | Its mean team place, two decimals. |
| `score` | number | always | Its summed team score. |
| `kills` | number | always | Its summed team kills. |
| `deaths` | number | always | Its summed team deaths. |

## 4. Exit codes

| Code | Meaning |
|---|---|
| 0 | Every match played (a capped match is a result, not an error); also `--help`. |
| 2 | Bad arguments, or content that fails to load or does not resolve (an unknown arena, mode, loadout or power-up, `--team` counts that do not fit the mode). |
| 3 | A module was refused: `WasmModule::load` failed (the reason on stderr), or a module spec in a build without `KFM_WASM_BOTS`. |
| 4 | I/O: a module file, the replay (`--replay`, `--replay-dir`) or the `--result` file could not be read or written. |

A reader treats any non-zero code as a failed run, whatever stdout holds: codes 2 and 3 come
before the first match, a module or replay failure (3, 4) during a match leaves no summary, and a
`--result` file that fails at the end (4) comes after it.

## 5. Versioning

- The promise is append-only: a key of any object above is never renamed, removed or retyped,
  and its meaning never changes. A *number* stays a number, a key that cannot be `null` never
  becomes nullable, and the conditions under which a key is present never narrow.
- A new key is added after the existing keys of its object (or of its conditional group: a new
  module key follows `dead`) and listed in the tables here with the architecture version that
  added it, in the same change, together with its frozen list in the test.
- A reader must ignore keys it does not know, and must not rely on the order of keys (the order
  is stable and pinned, but conditional keys sit in the middle: a team match's `team` follows a
  slot's `module`).
- A string with a closed set of values (`result`, `dead`) may gain values; a reader treats an
  unknown one as "something else", never as an error.
- What a value is computed from (the content, the sim) moves with every release: `checksum`,
  scores and fuel differ between releases. `content_hash` and `build_hash` name the content and
  the build a record came from, and the release tag the ladder records names the `kfm_arena`
  that re-runs it bit for bit.

| Version | Change |
|---|---|
| v0.36 | The match and summary objects declared append-only (as built in v0.30, teams added in v0.35). |
| v0.60 | `content_hash` and `build_hash` appended to the match object; this document and the frozen-key test. |
| v0.70 | No key changes: a release's `kfm_arena` stamps `build_hash` (its build id) instead of `"00000000"`. |
| v0.81 | `arena` appended to the match object: a mode's arena pool is cycled over the matches. |

## 6. Example

`kfm_arena --bots 2 --matches 1 --ticks 600 --json` (without `--quiet`; `--quiet` prints only
the last line). The hashes and numbers are those of the build and content this document was last
updated with; any release may move them:

```
{"match":0,"seed":1,"ticks":600,"checksum":"c2cfdc04","result":"cap","winner":null,"slots":[{"slot":0,"bot":0,"spec":"ref:hard","module":false,"place":1,"score":1,"kills":1,"deaths":0,"acts":600,"subs":0},{"slot":1,"bot":1,"spec":"ref:hard","module":false,"place":2,"score":0,"kills":0,"deaths":1,"acts":419,"subs":0}],"content_hash":"abbc5f45","build_hash":"00000000","arena":"dojo"}
{"summary":true,"matches":1,"ended":0,"draws":0,"cap":1,"ticks_mean":600,"slots":[{"slot":0,"spec":"ref:hard","module":false,"wins":0,"mean_place":1.00,"score":1,"kills":1,"deaths":0,"acts":600,"subs":0},{"slot":1,"spec":"ref:hard","module":false,"wins":0,"mean_place":2.00,"score":0,"kills":0,"deaths":1,"acts":419,"subs":0}]}
```
