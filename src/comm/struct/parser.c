#include "parser.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void *my_jsonv_arena_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void my_jsonv_arena_reset(void *user_data) {
  /*#region*/
  arena_reset((Arena *)user_data);
  /*#endregion*/
}

static void my_jsonv_arena_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  arena_restore((Arena *)user_data, keep_size);
  /*#endregion*/
}

static void my_jsonv_arena_destroy(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static const Jsonv_Arena_Ops my_jsonv_ops = {
  .alloc = my_jsonv_arena_alloc,
  .reset = my_jsonv_arena_reset,
  .reset_to = my_jsonv_arena_reset_to,
  .destroy = my_jsonv_arena_destroy
};

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

static const char *workflow_schema_json = 
  "{\n"
  "  \"type\": \"object\",\n"
  "  \"required\": [\"version\", \"name\", \"on\", \"jobs\"],\n"
  "  \"properties\": {\n"
  "    \"version\": { \"type\": \"string\" },\n"
  "    \"name\": { \"type\": \"string\", \"maxLength\": 128 },\n"
  "    \"on\": {\n"
  "      \"type\": \"object\",\n"
  "      \"minProperties\": 1\n"
  "    },\n"
  "    \"concurrency\": { \"type\": \"integer\" },\n"
  "    \"env\": {\n"
  "      \"type\": \"object\",\n"
  "      \"additionalProperties\": { \"type\": \"string\" }\n"
  "    },\n"
  "    \"jobs\": {\n"
  "      \"type\": \"object\",\n"
  "      \"additionalProperties\": {\n"
  "        \"type\": \"object\",\n"
  "        \"properties\": {\n"
  "          \"type\": { \"type\": \"string\", \"enum\": [\"task\", \"if\", \"switch\", \"fork\", \"join\", \"loop\", \"wait_signal\", \"wait_timer\"] },\n"
  "          \"name\": { \"type\": \"string\" },\n"
  "          \"depends_on\": {\n"
  "            \"anyOf\": [\n"
  "              { \"type\": \"string\" },\n"
  "              { \"type\": \"array\", \"items\": { \"type\": \"string\" } }\n"
  "            ]\n"
  "          },\n"
  "          \"steps\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": {\n"
  "              \"type\": \"object\",\n"
  "              \"properties\": {\n"
  "                \"id\": { \"type\": \"string\" },\n"
  "                \"http\": {\n"
  "                  \"type\": \"object\",\n"
  "                  \"required\": [\"method\", \"url\"],\n"
  "                  \"properties\": {\n"
  "                    \"method\": { \"type\": \"string\", \"enum\": [\"GET\", \"POST\", \"PUT\", \"PATCH\", \"DELETE\"] },\n"
  "                    \"url\": { \"type\": \"string\" },\n"
  "                    \"headers\": { \"type\": \"object\" },\n"
  "                    \"body\": { \"type\": \"object\" },\n"
  "                    \"timeout\": { \"type\": \"string\" },\n"
  "                    \"mtls_profile\": { \"type\": \"string\" }\n"
  "                  }\n"
  "                },\n"
  "                \"uses\": { \"type\": \"string\" },\n"
  "                \"with\": { \"type\": \"object\" },\n"
  "                \"timeout\": { \"type\": \"string\" },\n"
  "                \"retry_attempts\": { \"type\": \"integer\" },\n"
  "                \"retry_backoff\": { \"type\": \"string\" },\n"
  "                \"retry_delay\": { \"type\": \"string\" }\n"
  "              }\n"
  "            }\n"
  "          },\n"
  "          \"condition\": { \"type\": \"string\" },\n"
  "          \"then\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"else\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"cases\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": {\n"
  "              \"type\": \"object\",\n"
  "              \"required\": [\"condition\", \"then\"],\n"
  "              \"properties\": {\n"
  "                \"condition\": { \"type\": \"string\" },\n"
  "                \"then\": {\n"
  "                  \"type\": \"array\",\n"
  "                  \"items\": { \"type\": \"string\" }\n"
  "                }\n"
  "              }\n"
  "            }\n"
  "          },\n"
  "          \"default\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"branches\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"strategy\": { \"type\": \"string\" },\n"
  "          \"n_required\": { \"type\": \"integer\" },\n"
  "          \"loop_type\": { \"type\": \"string\", \"enum\": [\"while\", \"for_each\"] },\n"
  "          \"max_iterations\": { \"type\": \"integer\" },\n"
  "          \"items\": { \"type\": \"string\" },\n"
  "          \"correlation_id\": { \"type\": \"string\" },\n"
  "          \"duration\": { \"type\": \"string\" }\n"
  "        }\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "}\n";

int32_t parser_parse_buffer(Arena *arena, const char *buffer, size_t len, WorkflowAST *out_ast) {
  /*#region*/
  if (!arena || !buffer || !out_ast) {
    return ERR_OOM;
  }

  // Create jsonv arena via our custom allocator operations mapping directly into our Arena
  Jsonv_Arena *jsonv_arena = jsonv_arena_new_custom(&my_jsonv_ops, arena);
  if (!jsonv_arena) {
    return ERR_OOM;
  }

  // Create jsonv parser context
  Jsonv_Config config = {0};
  config.default_block_size = 4096;
  config.max_limit = 16 * 1024 * 1024;
  config.max_depth = 128;
  config.max_values = 10000;
  config.max_objects = 5000;
  config.max_array = 5000;
  config.max_string_bytes = 4 * 1024 * 1024;

  Jsonv_Arena_Error err_code = 0;
  Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
  if (!ctx) {
    return ERR_OOM;
  }

  Jsonv_Error schema_err = {0};
  Jsonv_Schema *schema = jsonv_schema_compile(jsonv_arena, (const unsigned char *)workflow_schema_json, &config, &schema_err);
  if (!schema) {
    fprintf(stderr, "Failed to compile workflow schema: %s\n", schema_err.description);
    if (schema_err.type == Jsonv_Mem_Failed || schema_err.type == Jsonv_Arena_Limit_Reached || schema_err.type == Jsonv_Arena_Overflow) {
      return ERR_OOM;
    }
    return ERR_MISSING_VAR;
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

  // Validate workflow against JSON schema
  if (!jsonv_ctx_validate(ctx, schema)) {
    const Jsonv_Error *val_err = jsonv_ctx_get_error(ctx);
    if (val_err) {
      fprintf(stderr, "Validation Error: %s at path %s\n", val_err->description, val_err->path ? val_err->path : "");
    }
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

  // Extract concurrency limit (if present)
  out_ast->max_concurrency = 0; // default to unlimited
  Jsonv_Value v_concurrency;
  if (jsonv_obj_get(root_val.as.p, "concurrency", &v_concurrency)) {
    if (v_concurrency.tag == JSONV_VAL_INT) {
      out_ast->max_concurrency = (int)v_concurrency.as.i;
    } else if (v_concurrency.tag == JSONV_VAL_DOUBLE) {
      out_ast->max_concurrency = (int)v_concurrency.as.d;
    }
  }

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
