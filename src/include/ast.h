#ifndef _NESTOR_AST_H
#define _NESTOR_AST_H

#include "stringview.h"
#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>

typedef struct {
  StringView key;
  StringView value;
} EnvVarAST;

typedef struct {
  const char *input_buffer;
  size_t input_len;
  Jsonv_Context *jsonv_ctx;
  Jsonv_Value root_val;

  StringView version;
  StringView name;

  EnvVarAST *env;
  size_t env_count;
} WorkflowAST;

#endif
