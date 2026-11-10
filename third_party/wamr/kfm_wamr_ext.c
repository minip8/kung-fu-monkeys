/* See kfm_wamr_ext.h. Built inside kfm_wamr's directory scope, so the
 * WASM_ENABLE_* definitions (and with them the layout of WASMExecEnv and
 * WASMModule) match the runtime it is linked with. */
#include "kfm_wamr_ext.h"

#include "wasm_exec_env.h"
#include "wasm.h"

#if WASM_ENABLE_INSTRUCTION_METERING == 0
#error "kfm_wamr must be built with WAMR_BUILD_INSTRUCTION_METERING=1"
#endif
#if WASM_ENABLE_AOT != 0 || WASM_ENABLE_JIT != 0 || WASM_ENABLE_FAST_JIT != 0
#error "kfm_wamr is classic-interpreter only: every wasm_module_t is a WASMModule"
#endif

/* Read by the patched wasm_exec_env_create_internal (wasm_exec_env.c). */
int kfm_wamr_default_instruction_limit = -1;

int kfm_wamr_instructions_left(wasm_exec_env_t exec_env) {
  return exec_env->instructions_to_execute;
}

void kfm_wamr_set_default_instruction_limit(int limit) {
  kfm_wamr_default_instruction_limit = limit;
}

uint32_t kfm_wamr_module_memory(wasm_module_t module, uint64_t* init_bytes, uint64_t* max_bytes) {
  const WASMModule* m = (const WASMModule*)module;
  const WASMMemory* mem = NULL;
  if (m->import_memory_count > 0) {
    mem = &m->import_memories[0].u.memory.mem_type;
  } else if (m->memory_count > 0) {
    mem = &m->memories[0];
  } else {
    return 0;
  }
  *init_bytes = (uint64_t)mem->init_page_count * mem->num_bytes_per_page;
  *max_bytes = (uint64_t)mem->max_page_count * mem->num_bytes_per_page;
  return m->import_memory_count + m->memory_count;
}

uint64_t kfm_wamr_module_table_elements(wasm_module_t module) {
  const WASMModule* m = (const WASMModule*)module;
  uint64_t total = 0;
  for (uint32_t i = 0; i < m->import_table_count; ++i) {
    const WASMTableType* t = &m->import_tables[i].u.table.table_type;
    total += t->possible_grow ? t->max_size : t->init_size;
  }
  for (uint32_t i = 0; i < m->table_count; ++i) {
    const WASMTableType* t = &m->tables[i].table_type;
    total += t->possible_grow ? t->max_size : t->init_size;
  }
  return total;
}
