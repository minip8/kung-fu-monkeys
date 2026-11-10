;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; table_fill: kfm_act(n) writes n table elements with one table.fill and returns 0. The
;; table holds 65536 elements, so filling it is a single opcode over a long host loop;
;; the kfm charge (one unit per element) is what bounds it. The test reads the slope off
;; small n and drives it over budget with the whole table.
(module
  (memory (export "memory") 2)
  (table 65536 funcref)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (table.fill (i32.const 0) (ref.null func) (local.get $n))
    (i64.const 0)))
