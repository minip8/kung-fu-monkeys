;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp).
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; abi_v0: says version 0, below every version the host speaks (AbiMismatch).
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 0))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64) (i64.const 0))
)
