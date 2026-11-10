;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; init_bulk: like bulk_fill but the endless 16 MiB memory.fill loop is in kfm_init, so
;; the init budget (not the per-tick budget) bounds it: a few fills, then over budget and
;; the agent is dead. Without the kfm charge this would stall the host at load/respawn.
(module
  (memory (export "memory") 256)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32)
    (loop $again
      (memory.fill (i32.const 0) (i32.const 0) (i32.const 0x1000000))
      (br $again)))
  (func (export "kfm_act") (param i32) (result i64) (i64.const 0)))
