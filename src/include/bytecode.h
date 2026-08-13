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
  char magic[4];               // Must be "NEST"
  uint16_t version;            // Target Nestor version (e.g. 4)
  uint16_t flags;              // Bit 0: Signed, Bit 1: Debug symbols present
  uint8_t signature[64];       // Ed25519 asymmetric signature of the binary
  
  uint32_t const_pool_offset;  // Offset to Constant Pool
  uint32_t const_pool_count;   // Number of entries in Constant Pool
  
  uint32_t wf_table_offset;    // Offset to Workflow/Function Table
  uint32_t wf_table_count;     // Number of workflows/functions bundled
  
  uint32_t code_offset;        // Offset to Instruction Segment
  uint32_t code_size;          // Size in bytes of Code Segment
} NVMHeader;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
  uint32_t name_const_idx;     // Constant pool index of the workflow/function name
  uint32_t entry_offset;       // Byte offset of the entry point in code segment
  uint32_t metadata_offset;    // File offset of the YAML/JSON metadata block
  uint32_t metadata_size;      // Size in bytes of the metadata block
} NVMWorkflowEntry;
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
#define OP_LOAD_ENV        0x0B
#define OP_STORE_VAR       0x0C
#define OP_COMPLETE_JOB    0x0D
#define OP_SKIP_JOB        0x0E

int32_t bytecode_compile_workspace(Arena *arena, WorkspaceMap *map, const char *output_nbc_path);
int32_t bytecode_compile_workflow(Arena *arena, WorkflowAST *ast, const char *output_nbc_path);
int32_t disassemble_nbc_file(Arena *arena, const char *nbc_file_path);

#endif // _NESTOR_BYTECODE_H
