;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; grow: every act grows memory by one page and returns memory.grow's result in the
;; low 32 bits (the old size in pages, or 0xffffffff once the cap is reached) and the
;; first byte of the observation's match.tick (byte 16) in byte 4, read after the
;; growth (the host must re-resolve the buffer, which may have moved). Not the header's
;; first byte: a version 1 module's header is the host's (architecture v0.80).
(module
  (memory (export "memory") 2)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64)
    (i64.or
      (i64.extend_i32_u (memory.grow (i32.const 1)))
      (i64.shl (i64.extend_i32_u (i32.load8_u (i32.const 65552))) (i64.const 32)))))
