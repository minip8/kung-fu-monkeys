;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; logger: logs "hello from wasm\n" (16 bytes) through env.kfm_log on every act;
;; on tick 999 it passes an out-of-bounds pointer instead, which traps.
(module
  (import "env" "kfm_log" (func $log (param i32 i32)))
  (memory (export "memory") 2)
  (data (i32.const 16) "hello from wasm\n")
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $tick i32) (result i64)
    (if (i32.eq (local.get $tick) (i32.const 999))
      (then (call $log (i32.const 0x7ffffff0) (i32.const 64)))
      (else (call $log (i32.const 16) (i32.const 16))))
    (i64.const 0)))
