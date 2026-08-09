#ifndef _NESTOR_RUNNER_H
#define _NESTOR_RUNNER_H

#include "arena.h"
#include "ast.h"
#include "error_codes.h"
#include "plugin.h"
#include "transport.h"

#include "response_buffer.h"

typedef struct IPCClient IPCClient;
struct IPCClient {
  int fd;
  char buf[1024];
  int len;
  IPCClient *next;
};

void process_ipc_request(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, int client_fd, const char *req_str);
bool jsonv_values_equal(Jsonv_Value a, Jsonv_Value b);
void mark_job_skipped(WorkflowAST *ast, StringView id);
void update_job_states(WorkflowAST *ast);
typedef struct ActiveJob ActiveJob;
int32_t start_loop_iteration(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj);
struct ActiveJob {
  JobNode *job;
  StepNode *curr_step;
  Jsonv_Value steps_state_obj;
  Jsonv_Value local_vars;

  // Loop support
  bool is_loop;
  size_t loop_iter;
  size_t max_iterations;
  Jsonv_Value loop_history_obj;
  char loop_stream_file_path[256];
  size_t loop_stream_record_offset;

  // Active HTTP transport fields
  void *easy_handle;
  ResponseBuffer resp_buf;

  // Active Plugin fields
  PluginExecutor plugin_exec;

  // Retry tracking fields
  int curr_step_retry_attempt;
  struct timeval next_retry_time;
  bool is_waiting_retry;

  Arena *loop_arena;

  // Caching/validation fields
  bool is_validating;
  char cached_key[65];
  char cached_etag[128];
  char cached_last_modified[128];
  char *cached_output_payload;

  ActiveJob *next;
};

typedef struct Transport Transport;
int32_t run_workflow(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val);
int32_t run_workflow_opt(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val, Transport *transport);
int32_t execute_step(Arena *arena, StepNode *step, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value steps_state_obj);

void push_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars);
void pop_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars);
int32_t evaluate_job_variables(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, JobNode *job, Jsonv_Value local_vars);
bool check_and_apply_cache(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, JobNode *job, ActiveJob *aj_out);
void store_job_in_cache(Arena *arena, WorkflowAST *ast, Jsonv_Value context_val, JobNode *job, ActiveJob *aj);
int32_t advance_active_job(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, ActiveJob *aj, Transport *transport);
bool handle_step_failure(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj, const char *err_msg);
int32_t apply_step_fallback(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj, WorkflowAST *ast, Transport *transport);
int32_t complete_http_step_async(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, long status_code);
int32_t complete_plugin_step_async(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, long exit_code);
bool is_edge_satisfied(JobNode *job, size_t d, JobNode *dep);
void get_state_paths(WorkflowAST *ast, char *state_path, size_t state_len, char *lock_path, size_t lock_len);
int32_t save_tfstate(Arena *arena, const char *state_path, Jsonv_Value context_val);

#endif
