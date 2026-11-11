;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp).
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; abi_v3: says version 3, the next one after the host's (architecture v0.79: a host that
;; speaks 1 and 2 refuses a third, AbiMismatch).
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 3))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64) (i64.const 0))
)
