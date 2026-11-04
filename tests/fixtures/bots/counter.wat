;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; counter: per-instance state. kfm_init stores the slot and the seed's low byte;
;; every act increments a cell in linear memory and returns
;; moveDir = count, moveMag = slot, aimDir = seed & 255.
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param $seed i64) (param $slot i32)
    (i32.store (i32.const 0) (i32.const 0))
    (i32.store (i32.const 4) (local.get $slot))
    (i32.store (i32.const 8) (i32.wrap_i64 (i64.and (local.get $seed) (i64.const 255)))))
  (func (export "kfm_act") (param i32) (result i64)
    (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
    (i64.or
      (i64.or
        (i64.extend_i32_u (i32.and (i32.load (i32.const 0)) (i32.const 255)))
        (i64.shl (i64.extend_i32_u (i32.and (i32.load (i32.const 4)) (i32.const 255)))
                 (i64.const 8)))
      (i64.shl (i64.extend_i32_u (i32.load (i32.const 8))) (i64.const 16)))))
