;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v2.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; tight_v2: tight_v1 saying version 2. Its buffer has room for version 1's 5748 bytes
;; and not version 2's 5880, so the host refuses it (BadObsBuffer).
(module
  (memory (export "memory") 2 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 2))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 125324))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $at i32) (result i64)
    (i64.extend_i32_u (i32.load (i32.add (i32.const 125324) (local.get $at))))))
