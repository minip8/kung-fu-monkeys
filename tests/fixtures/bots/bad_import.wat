;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; bad_import: imports something other than env.kfm_log.
(module
  (import "env" "fopen" (func $fopen (param i32 i32) (result i32)))
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64)
    (drop (call $fopen (i32.const 0) (i32.const 0)))
    (i64.const 0)))
