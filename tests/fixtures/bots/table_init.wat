;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; table_init: kfm_act(n) copies n elements from a 8-entry passive element segment into
;; the table with one table.init and returns 0. The kfm charge is one unit per element;
;; the test reads the slope off stats().fuelLast for n up to the segment length.
(module
  (memory (export "memory") 2)
  (table 65536 funcref)
  (func $noop)
  (elem $seg funcref
    (ref.func $noop) (ref.func $noop) (ref.func $noop) (ref.func $noop)
    (ref.func $noop) (ref.func $noop) (ref.func $noop) (ref.func $noop))
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (table.init $seg (i32.const 0) (i32.const 0) (local.get $n))
    (i64.const 0)))
