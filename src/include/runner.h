#ifndef _NESTOR_RUNNER_H
#define _NESTOR_RUNNER_H

#include "arena.h"
#include "ast.h"
#include "error_codes.h"

typedef struct {
  Arena *arena;
  char *buf;
  size_t len;
  size_t cap;
  char cache_control[256];
  char expires[128];
  char etag[128];
  char last_modified[128];
} ResponseBuffer;

typedef struct Transport Transport;
int32_t run_workflow(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val);
int32_t run_workflow_opt(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val, Transport *transport);
int32_t execute_step(Arena *arena, StepNode *step, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value steps_state_obj);

#endif
