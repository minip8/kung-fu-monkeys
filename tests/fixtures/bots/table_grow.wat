;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; table_grow: kfm_act(n) grows the table by n elements and returns the old size (or
;; 0xffffffff when n would exceed the table's maximum, which does no work and is not
;; charged). WAMR fills the n new elements at grow time; the kfm charge is one unit per
;; element added. The table starts at 20000 elements and grows to at most 40000 (WAMR
;; clamps a growable table's maximum to about twice its initial size). The test compares a
;; fresh instance grown by 0 against one grown by 10000 to read the per-element charge.
(module
  (memory (export "memory") 2)
  (table 20000 40000 funcref)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (i64.extend_i32_u (table.grow (ref.null func) (local.get $n)))))
