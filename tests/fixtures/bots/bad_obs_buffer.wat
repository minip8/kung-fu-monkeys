;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; bad_obs_buffer: points the observation buffer at the end of its memory (no room for any byte).
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 131072))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64) (i64.const 0))
)
