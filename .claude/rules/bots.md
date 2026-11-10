---
paths:
  - "engine/ai/**"
  - "apps/arena/**"
  - "tools/bots/**"
  - "third_party/wamr/**"
  - "tests/unit/ai/**"
  - "tests/unit/arena/**"
  - "tests/fixtures/bots/**"
  - "docs/bot-abi.md"
  - "docs/arena-result.md"
  - "apps/server/server_host.*"
  - "apps/client/core/match_session.*"
  - "apps/client/core/bot_roster.*"
---

# Bot rules

These add to the bot contract in `CLAUDE.md` ("Bots (§8)"), which always applies.

- Fog lives in `engine/ai`, never in the sim: a `vision` change is content hash only, and the
  `all` vision must stay the pre-fog observation byte for byte (a test pins it). Hosts call
  `AgentHost::observe(sink, world, content)` after every `step`, or their bots hear no calls
  (a host that steps the world itself calls `AgentHost::step`, which does fill, step and
  observe; `fill()` asserts the previous step was observed). Agents are made by `ai::BotFactory`
  (`kfm::ai_bots`) and seeded by `ai::agentSeed`, never by a per-host copy.
- The runtime is WAMR 2.4.5's classic interpreter (`kfm::wamr`, FetchContent only, patched at
  configure time to read the remaining budget back; `KFM_WASM_BOTS=OFF` builds without it, and
  Emscripten never builds it). `kfm_ai_wasm` is a separate target so `kfm_ai` never sees WAMR.
  The budgets are 100 k instructions per tick and 10 M for instantiation and `kfm_init`; modules
  are at most 4 MiB with 16 MiB of memory.
- `tools/bots/sdk-c/kfm_bot.h` is copied byte for byte by the bots repository, whose CI diffs
  it: a change to it or to `docs/bot-abi.md` is a change to a public contract.
- The `kfm_arena --json` objects are an append-only contract (`docs/arena-result.md`,
  normative; a test freezes every key, its order and type): a new key is appended to the code,
  the doc and the test, never inserted or renamed.
