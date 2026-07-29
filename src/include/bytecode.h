#ifndef _NESTOR_BYTECODE_H
#define _NESTOR_BYTECODE_H

#include "arena.h"
#include "ast.h"
#include "loader.h"
#include "error_codes.h"
#include <stdint.h>
#include <stddef.h>

#pragma pack(push, 1)
typedef struct {
  char magic[4];              // Must be "NEST"
  uint16_t version;           // e.g., 3
  uint16_t flags;             // e.g., 0
  uint32_t const_pool_offset; // Offset to Constant Pool
  uint32_t const_pool_count;  // Number of entries
  uint32_t code_offset;       // Offset to Code Segment
  uint32_t code_size;         // Size in bytes of Code Segment
} NVMHeader;
#pragma pack(pop)

// Opcodes defined in ISA Specification
#define OP_NOP             0x00
#define OP_PUSH_CONST      0x01
#define OP_POP             0x02
#define OP_RESOLVE         0x03
#define OP_JUMP            0x04
#define OP_JUMP_IF_FALSE   0x05
#define OP_CALL_PROVIDER   0x06
#define OP_CALL_WORKFLOW   0x07
#define OP_RETURN          0x08
#define OP_FORK            0x09
#define OP_JOIN            0x0A

int32_t bytecode_compile_workspace(Arena *arena, WorkspaceMap *map, const char *output_nbc_path);
int32_t bytecode_compile_workflow(Arena *arena, WorkflowAST *ast, const char *output_nbc_path);

#endif // _NESTOR_BYTECODE_H
