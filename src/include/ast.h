#ifndef _NESTOR_AST_H
#define _NESTOR_AST_H

#include "stringview.h"
#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>

typedef struct {
  StringView key;
  StringView value;
} EnvVarAST;

typedef enum {
  VAR_PRIVATE,
  VAR_PUBLIC
} VarVisibility;

typedef struct VariableAST VariableAST;
struct VariableAST {
  StringView name;
  StringView expression;
  VarVisibility visibility;
  VariableAST *next;
};

#define DEP_COND_SUCCESS    (1 << 0)
#define DEP_COND_FAILURE    (1 << 1)
#define DEP_COND_SKIP       (1 << 2)
#define DEP_COND_COMPLETION (DEP_COND_SUCCESS | DEP_COND_FAILURE | DEP_COND_SKIP)

typedef enum {
  NODE_TASK,
  NODE_IF,
  NODE_SWITCH,
  NODE_FORK,
  NODE_JOIN,
  NODE_LOOP,
  NODE_WAIT_SIGNAL,
  NODE_WAIT_TIMER,
  NODE_TRANSFORM
} NodeType;

typedef enum {
  STATE_PENDING = 0,
  STATE_RUNNING = 1,
  STATE_SUCCEEDED = 2,
  STATE_FAILED = 3,
  STATE_SKIPPED = 4,
  STATE_SUSPENDED = 5
} JobState;

typedef struct StepNode StepNode;
struct StepNode {
  StringView id;
  VariableAST *variables_head;
  VariableAST *outputs_head;
  bool is_http; // true for http, false for plugin/uses
  StringView timeout;
  int retry_attempts;
  StringView retry_backoff;
  StringView retry_delay;

  struct {
    StringView method;
    StringView url;
    Jsonv_Value headers;
    Jsonv_Value body;
    StringView mtls_profile;
    bool stream;
    int chunk_size;
  } http;

  struct {
    StringView uses;
    Jsonv_Value with_args;
    bool sandboxed;
  } plugin;

  StepNode *next;
};

typedef struct SwitchCase SwitchCase;
struct SwitchCase {
  StringView condition;
  StringView *then_branch;
  size_t then_count;
  SwitchCase *next;
};

typedef struct JobNode JobNode;
struct JobNode {
  NodeType type;
  StringView id;
  StringView name;
  VariableAST *variables_head;

  // Intrusive topological graph pointers
  JobNode *next_sorted;
  JobNode **depends_on_nodes;  // Resolved upstream dependency node pointers
  StringView *depends_on_ids;  // Raw upstream dependency IDs
  uint8_t *depends_on_conditions; // Bitmask array for dependency conditions
  size_t dependency_count;

  // Bitwise/byte execution state tracking
  uint8_t execution_state; // PENDING, RUNNING, SUCCEEDED, FAILED, SKIPPED, SUSPENDED

  bool is_start;
  bool is_end;
  StringView return_expr;

  union {
    struct {
      StepNode *steps_head;
    } task;

    struct {
      StringView condition;
      StringView *then_branch;
      size_t then_count;
      StringView *else_branch;
      size_t else_count;
    } binary_if;

    struct {
      SwitchCase *cases;
      size_t case_count;
      StringView *default_branch;
      size_t default_count;
    } multi_switch;

    struct {
      StringView *branches;
      size_t branch_count;
    } fork_node;

    struct {
      StringView strategy; // "all", "any", "n_required"
      size_t n_required;
    } join_node;

    struct {
      StringView loop_type; // "while", "for_each", "stream_chunk"
      StringView condition;
      StringView items;
      size_t max_iterations;
      StepNode *steps_head;
      StringView source;
      size_t chunk_record_limit;
    } loop_node;

    struct {
      StringView correlation_id;
      StringView timeout;
      StepNode *steps_head;
    } wait_signal;

    struct {
      StringView duration;
    } wait_timer;

    struct {
      StringView expression;
    } transform;
  } spec;
};

typedef struct {
  const char *input_buffer;
  size_t input_len;
  Jsonv_Context *jsonv_ctx;
  Jsonv_Value root_val;

  StringView version;
  StringView name;

  EnvVarAST *env;
  size_t env_count;

  VariableAST *variables_head;

  JobNode *jobs_head; // Topologically sorted JobNodes list
  size_t job_count;
  int max_concurrency;
  char ipc_socket_path[256];
} WorkflowAST;

#endif
