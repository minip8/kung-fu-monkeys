;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; echo: a fixed input read off the tick: moveDir 64, moveMag 255, aimDir = tick & 255,
;; and on even ticks Throw (buttons bit 0) with subTick 128.
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $tick i32) (result i64)
    (local $even i64)
    (local.set $even
      (i64.extend_i32_u (i32.eqz (i32.and (local.get $tick) (i32.const 1)))))
    (i64.or
      (i64.or
        (i64.const 0xff40)
        (i64.shl (i64.extend_i32_u (i32.and (local.get $tick) (i32.const 255))) (i64.const 16)))
      (i64.or
        (i64.shl (local.get $even) (i64.const 24))
        (i64.shl (i64.mul (local.get $even) (i64.const 128)) (i64.const 32))))))
