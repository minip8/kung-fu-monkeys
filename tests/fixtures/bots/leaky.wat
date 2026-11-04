;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; leaky: a compiled-code shadow stack. Every act pushes a 16-byte frame by moving the
;; exported __stack_pointer down, traps on odd ticks before popping it, and otherwise
;; pops it and returns the pointer's value at entry in the low 32 bits. The host
;; restores the pointer after a fault, so the value never drifts.
(module
  (memory (export "memory") 2)
  (global $sp (export "__stack_pointer") (mut i32) (i32.const 32768))
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $tick i32) (result i64)
    (local $entry i32)
    (local.set $entry (global.get $sp))
    (global.set $sp (i32.sub (local.get $entry) (i32.const 16)))
    (if (i32.and (local.get $tick) (i32.const 1))
      (then (unreachable)))
    (global.set $sp (local.get $entry))
    (i64.extend_i32_u (local.get $entry))))
