#include "bytecode.h"
#include "compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char strings[256][128];
  uint32_t count;
} ConstPoolBuilder;

static uint32_t add_constant(ConstPoolBuilder *cp, const char *str, size_t len) {
  /*#region*/
  for (uint32_t i = 0; i < cp->count; i++) {
    if (strncmp(cp->strings[i], str, len) == 0 && cp->strings[i][len] == '\0') {
      return i;
    }
  }
  if (cp->count >= 256) return 0;
  uint32_t idx = cp->count++;
  size_t copy_len = len < 127 ? len : 127;
  memcpy(cp->strings[idx], str, copy_len);
  cp->strings[idx][copy_len] = '\0';
  return idx;
  /*#endregion*/
}

int32_t bytecode_compile_workflow(Arena *arena, WorkflowAST *ast, const char *output_nbc_path) {
  /*#region*/
  (void)arena;
  if (!ast || !output_nbc_path) return ERR_INVALID_BOUNDARY;

  ConstPoolBuilder cp = {0};
  add_constant(&cp, "NEST_INIT", 9);

  uint8_t code_buf[4096];
  uint32_t code_size = 0;

  // Generate basic sequence of instructions for workflow jobs
  JobNode *job = ast->jobs_head;
  while (job) {
    uint32_t id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
    code_buf[code_size++] = OP_PUSH_CONST;
    code_buf[code_size++] = (id_const >> 24) & 0xFF;
    code_buf[code_size++] = (id_const >> 16) & 0xFF;
    code_buf[code_size++] = (id_const >> 8) & 0xFF;
    code_buf[code_size++] = id_const & 0xFF;

    if (job->type == NODE_TASK) {
      code_buf[code_size++] = OP_CALL_PROVIDER;
      code_buf[code_size++] = 0; code_buf[code_size++] = 0; code_buf[code_size++] = 0; code_buf[code_size++] = id_const;
    }
    job = job->next_sorted;
  }
  code_buf[code_size++] = OP_RETURN;

  // Build Constant Pool binary buffer
  uint8_t const_buf[8192];
  uint32_t const_offset_bytes = 0;
  for (uint32_t i = 0; i < cp.count; i++) {
    uint32_t slen = (uint32_t)strlen(cp.strings[i]);
    const_buf[const_offset_bytes++] = (slen >> 24) & 0xFF;
    const_buf[const_offset_bytes++] = (slen >> 16) & 0xFF;
    const_buf[const_offset_bytes++] = (slen >> 8) & 0xFF;
    const_buf[const_offset_bytes++] = slen & 0xFF;
    memcpy(const_buf + const_offset_bytes, cp.strings[i], slen);
    const_offset_bytes += slen;
  }

  // Header setup
  NVMHeader header;
  memcpy(header.magic, "NEST", 4);
  header.version = 3;
  header.flags = 0;
  header.const_pool_offset = sizeof(NVMHeader);
  header.const_pool_count = cp.count;
  header.code_offset = sizeof(NVMHeader) + const_offset_bytes;
  header.code_size = code_size;

  FILE *f = fopen(output_nbc_path, "wb");
  if (!f) return ERR_HTTP_TRANSPORT;

  fwrite(&header, 1, sizeof(NVMHeader), f);
  if (const_offset_bytes > 0) {
    fwrite(const_buf, 1, const_offset_bytes, f);
  }
  if (code_size > 0) {
    fwrite(code_buf, 1, code_size, f);
  }
  fclose(f);

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t bytecode_compile_workspace(Arena *arena, WorkspaceMap *map, const char *output_nbc_path) {
  /*#region*/
  if (!map || !map->root_workflow) return ERR_INVALID_BOUNDARY;
  int32_t compile_res = compile_workflow(arena, &map->root_workflow->ast);
  if (compile_res != ERR_SUCCESS) return compile_res;
  return bytecode_compile_workflow(arena, &map->root_workflow->ast, output_nbc_path);
  /*#endregion*/
}
