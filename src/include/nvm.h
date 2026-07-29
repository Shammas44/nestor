#ifndef _NESTOR_NVM_H
#define _NESTOR_NVM_H

#include "arena.h"
#include "bytecode.h"
#include "error_codes.h"
#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>
#include <stdint.h>
#include <stddef.h>

#define ERR_VM_OUT_OF_BOUNDS     -20
#define ERR_VM_STACK_OVERFLOW    -21
#define ERR_VM_STACK_UNDERFLOW   -22
#define ERR_VM_INVALID_JUMP      -23
#define ERR_VM_ILLEGAL_INSTRUCTION -24

#define VM_STACK_LIMIT 256
#define VM_CALL_STACK_LIMIT 64

typedef struct NVMCallFrame NVMCallFrame;
struct NVMCallFrame {
  uint32_t return_pc;
  Jsonv_Value frame_inputs;
  NVMCallFrame *prev;
};

typedef struct {
  Arena *arena;
  Jsonv_Arena *jsonv_arena;

  const uint8_t *mapped_file;
  size_t mapped_size;

  const NVMHeader *header;
  const uint8_t *const_pool;
  const uint8_t *code_segment;

  uint32_t pc;
  uint32_t sp;
  Jsonv_Value stack[VM_STACK_LIMIT];

  NVMCallFrame *call_stack_top;
  size_t call_stack_depth;
} NVMContext;

int32_t nvm_init_from_file(NVMContext *ctx, Arena *arena, Jsonv_Arena *jsonv_arena, const char *nbc_file_path);
int32_t nvm_execute_loop(NVMContext *ctx);
void nvm_close(NVMContext *ctx);

#endif // _NESTOR_NVM_H
