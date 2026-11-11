;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; tight_v1: a version 1 module whose observation buffer ends exactly at the end of its
;; memory, 5748 bytes before it (architecture v0.79: version 1's observation is all a
;; version 1 module needs room for). kfm_act returns the buffer's u32 at byte `tick`.
(module
  (memory (export "memory") 2 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 125324))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $at i32) (result i64)
    (i64.extend_i32_u (i32.load (i32.add (i32.const 125324) (local.get $at))))))
