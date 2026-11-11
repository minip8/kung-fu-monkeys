;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v2.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; peek_v2: a version 2 module (architecture v0.79) that returns the u32 at byte `tick`
;; of its observation buffer, so the test reads back what the host wrote. The 256 bytes
;; after version 1's 5748 are 0xA5 from the start: the host writes the 132 of version
;; 2's zones block over the first of them and nothing past 5880.
(module
  (memory (export "memory") 2)
  (data (i32.const 71284) "\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5\a5")
  (func (export "kfm_abi_version") (result i32) (i32.const 2))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $at i32) (result i64)
    (i64.extend_i32_u (i32.load (i32.add (i32.const 65536) (local.get $at))))))
