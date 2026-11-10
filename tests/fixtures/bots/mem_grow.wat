;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; mem_grow: kfm_act(n) grows linear memory by n pages and returns the old page count (or
;; 0xffffffff when n would exceed the cap, which does no work and is not charged). The kfm
;; charge is 8192 units per page added (what filling the page would cost), so a module
;; cannot churn the allocator for free. The test compares a fresh instance grown by 1 page
;; against one grown by several to read the per-page charge.
(module
  (memory (export "memory") 2 256)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param $n i32) (result i64)
    (i64.extend_i32_u (memory.grow (local.get $n)))))
