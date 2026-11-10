;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; table_copy: kfm_act(n) copies n table elements with one table.copy and returns 0. The
;; kfm charge is one unit per element; the test reads the slope off stats().fuelLast.
(module
  (memory (export "memory") 2)
  (table 65536 funcref)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (table.copy (i32.const 0) (i32.const 0) (local.get $n))
    (i64.const 0)))
