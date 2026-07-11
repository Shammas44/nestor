#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static _Thread_local Arena *g_parser_arena = NULL;

static void *jsonv_arena_malloc(size_t size) {
  /*#region*/
  return na_alloc(g_parser_arena, size);
  /*#endregion*/
}

static void *jsonv_arena_calloc(size_t count, size_t size) {
  /*#region*/
  size_t total = count * size;
  void *ptr = na_alloc(g_parser_arena, total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
  /*#endregion*/
}

static void jsonv_arena_free(void *ptr) {
  /*#region*/
  (void)ptr;
  /*#endregion*/
}

static bool is_digit(char c) {
  /*#region*/
  return c >= '0' && c <= '9';
  /*#endregion*/
}

static bool validate_semver(StringView sv) {
  /*#region*/
  if (sv.length == 0 || !sv.data)
    return false;
  size_t i = 0;
  
  // Read major
  if (i >= sv.length || !is_digit(sv.data[i]))
    return false;
  while (i < sv.length && is_digit(sv.data[i]))
    i++;
  
  // Dot
  if (i >= sv.length || sv.data[i] != '.')
    return false;
  i++;
  
  // Read minor
  if (i >= sv.length || !is_digit(sv.data[i]))
    return false;
  while (i < sv.length && is_digit(sv.data[i]))
    i++;
  
  // Dot
  if (i >= sv.length || sv.data[i] != '.')
    return false;
  i++;
  
  // Read patch
  if (i >= sv.length || !is_digit(sv.data[i]))
    return false;
  while (i < sv.length && is_digit(sv.data[i]))
    i++;
  
  return i == sv.length;
  /*#endregion*/
}

int32_t parser_parse_buffer(Arena *arena, const char *buffer, size_t len, WorkflowAST *out_ast) {
  /*#region*/
  if (!arena || !buffer || !out_ast) {
    return ERR_OOM;
  }

  // Set up thread-local parser arena and jsonv custom allocators
  g_parser_arena = arena;
  g_jsonv_malloc = jsonv_arena_malloc;
  g_jsonv_calloc = jsonv_arena_calloc;
  g_jsonv_free = jsonv_arena_free;

  // Create jsonv arena. Because we redirected malloc/calloc, this will allocate inside our Arena.
  Jsonv_Arena *jsonv_arena = jsonv_arena_new(4096, 1024 * 1024, 12 * 1024);
  if (!jsonv_arena) {
    return ERR_OOM;
  }

  // Create jsonv parser context
  Jsonv_Config config = {0};
  Jsonv_Arena_Error err_code = 0;
  Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
  if (!ctx) {
    return ERR_OOM;
  }

  // Parse YAML/JSON data
  bool success = jsonv_ctx_parse_yaml_data(ctx, (const unsigned char *)buffer);
  if (!success) {
    // If YAML fails, try JSON
    success = jsonv_ctx_parse_data(ctx, (const unsigned char *)buffer);
  }

  if (!success) {
    return ERR_MISSING_VAR;
  }

  Jsonv_Value root_val;
  if (!jsonv_ctx_get_value(ctx, &root_val)) {
    return ERR_MISSING_VAR;
  }

  if (root_val.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }

  out_ast->input_buffer = buffer;
  out_ast->input_len = len;
  out_ast->jsonv_ctx = ctx;
  out_ast->root_val = root_val;

  // Extract version
  Jsonv_Value v_version;
  if (!jsonv_obj_get(root_val.as.p, "version", &v_version)) {
    return ERR_MISSING_VAR;
  }
  if (v_version.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  out_ast->version.data = (const char *)v_version.as.p;
  out_ast->version.length = jsonv_val_str_len(v_version);

  if (!validate_semver(out_ast->version)) {
    return ERR_MISSING_VAR;
  }

  // Extract name
  Jsonv_Value v_name;
  if (!jsonv_obj_get(root_val.as.p, "name", &v_name)) {
    return ERR_MISSING_VAR;
  }
  if (v_name.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  out_ast->name.data = (const char *)v_name.as.p;
  out_ast->name.length = jsonv_val_str_len(v_name);

  // Extract global env keys
  Jsonv_Value v_env;
  if (jsonv_obj_get(root_val.as.p, "env", &v_env) && v_env.tag == JSONV_VAL_OBJ) {
    Jsonv_Obj *env_obj = v_env.as.p;
    int count = jsonv_obj_length(env_obj);
    if (count > 0) {
      out_ast->env = na_alloc(arena, count * sizeof(EnvVarAST));
      if (!out_ast->env)
        return ERR_OOM;
      out_ast->env_count = count;

      for (int i = 0; i < count; i++) {
        const char *key = jsonv_obj_key_at(env_obj, i);
        Jsonv_Value val = jsonv_obj_val_at(env_obj, i);
        
        out_ast->env[i].key.data = key;
        out_ast->env[i].key.length = strlen(key);

        if (val.tag == JSONV_VAL_STRING) {
          out_ast->env[i].value.data = (const char *)val.as.p;
          out_ast->env[i].value.length = jsonv_val_str_len(val);
        } else {
          out_ast->env[i].value.data = "";
          out_ast->env[i].value.length = 0;
        }
      }
    } else {
      out_ast->env = NULL;
      out_ast->env_count = 0;
    }
  } else {
    out_ast->env = NULL;
    out_ast->env_count = 0;
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t parser_parse_stdin(Arena *arena, WorkflowAST *out_ast) {
  /*#region*/
  if (!arena || !out_ast)
    return ERR_OOM;

  // Allocate 1MB for reading standard input sequentially
  size_t capacity = 1024 * 1024;
  char *buffer = na_alloc(arena, capacity);
  if (!buffer)
    return ERR_OOM;

  size_t len = 0;
  int c;
  while ((c = fgetc(stdin)) != EOF) {
    if (len >= capacity - 1) {
      return ERR_OOM;
    }
    buffer[len++] = (char)c;
  }
  buffer[len] = '\0';

  return parser_parse_buffer(arena, buffer, len, out_ast);
  /*#endregion*/
}

int32_t parser_parse_file(Arena *arena, const char *filepath, WorkflowAST *out_ast) {
  /*#region*/
  if (!arena || !filepath || !out_ast)
    return ERR_OOM;

  FILE *f = fopen(filepath, "r");
  if (!f)
    return ERR_MISSING_VAR;

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size < 0) {
    fclose(f);
    return ERR_MISSING_VAR;
  }

  char *buffer = na_alloc(arena, size + 1);
  if (!buffer) {
    fclose(f);
    return ERR_OOM;
  }

  size_t read_bytes = fread(buffer, 1, size, f);
  buffer[read_bytes] = '\0';
  fclose(f);

  return parser_parse_buffer(arena, buffer, read_bytes, out_ast);
  /*#endregion*/
}
