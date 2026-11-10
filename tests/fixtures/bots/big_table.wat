;; Test fixture for kfm_ai_wasm_tests (tests/unit/ai/wasm_agent_test.cpp), bot ABI v1.
;; Regenerate the .wasm beside it with tools/bots/build_fixtures.sh (wabt's wat2wasm).
;; big_table: declares a table larger than the host's element cap. Instantiation would
;; allocate and fill it outside any instruction budget, so the host refuses it at load
;; (LoadErrorKind::TableTooLarge), the way it refuses an over-large linear memory.
(module
  (memory (export "memory") 2)
  (table 70000 funcref)
  (func (export "kfm_abi_version") (result i32) (i32.const 1))
  (func (export "kfm_obs_buffer") (result i32) (i32.const 65536))
  (func (export "kfm_init") (param i64 i32))
  (func (export "kfm_act") (param i32) (result i64) (i64.const 0)))
