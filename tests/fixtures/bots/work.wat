;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; work: kfm_act(tick) runs `tick` iterations of an integer mixing loop and returns 0.
;; Fuel use grows linearly with the tick; the throughput measurement times it.
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (local $x i32)
    (local.set $x (i32.const 1))
    (block $done
      (loop $next
        (br_if $done (i32.eqz (local.get $n)))
        (local.set $x (i32.add (i32.mul (local.get $x) (i32.const 1103515245)) (i32.const 12345)))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (br $next)))
    (i32.store (i32.const 0) (local.get $x))
    (i64.const 0)))
