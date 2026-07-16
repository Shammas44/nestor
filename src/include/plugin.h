#ifndef _NESTOR_PLUGIN_H
#define _NESTOR_PLUGIN_H

#include "arena.h"
#include "ast.h"
#include "runner.h"
#include "jsonv/jsonv.h"
#include <sys/types.h>
#include <sys/time.h>
#include <stdbool.h>

typedef struct PluginExecutor PluginExecutor;
struct PluginExecutor {
  pid_t child_pid;
  int child_stdout_fd;
  int child_stderr_fd;
  ResponseBuffer child_resp_buf;
  ResponseBuffer child_stderr_buf;
  long child_timeout_ms;
  struct timeval child_start_time;
  char *temp_in_path;
};

int32_t plugin_start(PluginExecutor *pe, Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value context_val, StepNode *step);
int32_t plugin_poll(PluginExecutor *pe, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, StepNode *step, bool *finished, long *exit_code);
void plugin_cleanup(PluginExecutor *pe);

#endif
