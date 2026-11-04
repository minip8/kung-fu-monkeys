;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; spin: every act loops forever; only the fuel budget stops it.
;; On odd ticks it returns at once (moveDir 1), so recovery after an over-budget tick shows.
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $tick i32) (result i64)
    (if (i32.and (local.get $tick) (i32.const 1))
      (then (return (i64.const 1))))
    (loop $forever
      (br $forever))
    (i64.const 0)))
