;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; bulk_fill: a hostile module. kfm_act fills the whole 16 MiB linear memory with
;; memory.fill in an endless loop. Un-metered that is a few opcodes per iteration
;; driving a 16 MiB memset each, hundreds of GiB of host work per tick; the kfm fuel
;; charge (len/8 per fill) makes even a single fill cost more than any tick budget, so
;; the first fill never runs and the tick is over budget after bounded work.
(module
  (memory (export "memory") 256)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64)
    (loop $again
      (memory.fill (i32.const 0) (i32.const 0) (i32.const 0x1000000))
      (br $again))
    (i64.const 0)))
