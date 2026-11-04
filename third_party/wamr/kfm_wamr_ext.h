/* kfm_wamr_ext: the three things the kfm wasm bot host (engine/ai,
 * WasmAgent) needs from WAMR that its public API (wasm_export.h) does not
 * offer. Compiled into kfm_wamr with WAMR's own configuration macros, so it
 * may read WAMR's internal structs; callers see only this header. Two of
 * them rely on the source patch applied by third_party/wamr/CMakeLists.txt. */
#ifndef KFM_WAMR_EXT_H
#define KFM_WAMR_EXT_H

#include <stdint.h>

#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The instruction budget left in `exec_env` after its last
 * wasm_runtime_call_wasm: the limit set with
 * wasm_runtime_set_instruction_count_limit minus the instructions executed
 * (0 when the call stopped on "instruction limit exceeded"; -1 when the
 * limit was -1, i.e. unmetered). WAMR 2.4.5 keeps the counter in a local of
 * the interpreter loop and never stores it back; the kfm patch writes it to
 * exec_env->instructions_to_execute when the loop returns. */
int kfm_wamr_instructions_left(wasm_exec_env_t exec_env);

/* The instruction limit every new exec_env starts with (WAMR's default is
 * -1, unmetered). wasm_runtime_instantiate runs the module's start function
 * and an exported __wasm_call_ctors on an exec_env it creates internally,
 * which the public API cannot meter; the host sets this around
 * instantiation so that code is bounded too (kfm patch). Process-wide:
 * single-threaded use only. */
void kfm_wamr_set_default_instruction_limit(int limit);

/* The declared initial and maximum size, in bytes, of the module's linear
 * memory (memory 0, imported or defined). Returns the number of memories the
 * module declares (0: neither output is written). Read before
 * instantiation, so a module asking for more than the host's cap is refused
 * without allocating anything. */
uint32_t kfm_wamr_module_memory(wasm_module_t module, uint64_t* init_bytes, uint64_t* max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* KFM_WAMR_EXT_H */
