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
  NODE_TASK,
  NODE_IF,
  NODE_SWITCH,
  NODE_FORK,
  NODE_JOIN,
  NODE_LOOP,
  NODE_WAIT_SIGNAL,
  NODE_WAIT_TIMER
} NodeType;

typedef struct StepNode StepNode;
struct StepNode {
  StringView id;
  bool is_http; // true for http, false for plugin/uses

  struct {
    StringView method;
    StringView url;
    Jsonv_Value headers;
    Jsonv_Value body;
    StringView timeout;
    StringView mtls_profile;
  } http;

  struct {
    StringView uses;
    Jsonv_Value with_args;
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

  // Intrusive topological graph pointers
  JobNode *next_sorted;
  JobNode **depends_on_nodes;  // Resolved upstream dependency node pointers
  StringView *depends_on_ids;  // Raw upstream dependency IDs
  size_t dependency_count;

  // Bitwise/byte execution state tracking
  uint8_t execution_state; // PENDING, RUNNING, SUCCEEDED, FAILED, SKIPPED, SUSPENDED

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
      StringView loop_type; // "while", "for_each"
      StringView condition;
      StringView items;
      size_t max_iterations;
      StepNode *steps_head;
    } loop_node;

    struct {
      StringView correlation_id;
      StringView timeout;
      StepNode *steps_head;
    } wait_signal;

    struct {
      StringView duration;
    } wait_timer;
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

  JobNode *jobs_head; // Topologically sorted JobNodes list
  size_t job_count;
} WorkflowAST;

#endif
