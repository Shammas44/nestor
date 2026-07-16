#include "nestor.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static char *allocate_jsonv_string(Arena *arena, const char *str) {
  /*#region*/
  size_t len = strlen(str);
  size_t total_size = sizeof(uint32_t) + len + 1;
  char *buf = na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)len;
  char *str_ptr = buf + sizeof(uint32_t);
  memcpy(str_ptr, str, len);
  str_ptr[len] = '\0';
  return str_ptr;
  /*#endregion*/
}

static void print_jsonv_value(Jsonv_Value v) {
  /*#region*/
  switch (v.tag) {
    case JSONV_VAL_UNDEFINED:
    case JSONV_VAL_NULL:
      printf("null");
      break;
    case JSONV_VAL_BOOLEAN:
      printf(v.as.boolean ? "true" : "false");
      break;
    case JSONV_VAL_INT:
      printf("%lld", (long long)v.as.i);
      break;
    case JSONV_VAL_DOUBLE:
      printf("%g", v.as.d);
      break;
    case JSONV_VAL_STRING:
      printf("\"%.*s\"", (int)jsonv_val_str_len(v), (const char *)v.as.p);
      break;
    case JSONV_VAL_ARRAY: {
      printf("[");
      int len = jsonv_arr_length(v.as.p);
      for (int i = 0; i < len; i++) {
        Jsonv_Value item;
        if (jsonv_arr_get(v.as.p, i, &item)) {
          print_jsonv_value(item);
        } else {
          printf("null");
        }
        if (i < len - 1) printf(", ");
      }
      printf("]");
      break;
    }
    case JSONV_VAL_OBJ: {
      printf("{");
      int len = jsonv_obj_length(v.as.p);
      for (int i = 0; i < len; i++) {
        const char *key = jsonv_obj_key_at(v.as.p, i);
        Jsonv_Value val = jsonv_obj_val_at(v.as.p, i);
        printf("\"%s\": ", key);
        print_jsonv_value(val);
        if (i < len - 1) printf(", ");
      }
      printf("}");
      break;
    }
    default:
      printf("null");
      break;
  }
  /*#endregion*/
}

#define CHUNK_SIZE 4096
static char *read_stdin(Arena *arena, size_t *out_len) {
  /*#region*/
  size_t capacity = CHUNK_SIZE;
  size_t length = 0;
  char *buf = na_alloc(arena, capacity);
  if (!buf) return NULL;

  while (1) {
    size_t n = fread(buf + length, 1, CHUNK_SIZE, stdin);
    if (n == 0) break;
    length += n;
    if (length + CHUNK_SIZE > capacity) {
      capacity *= 2;
      char *new_buf = na_alloc(arena, capacity);
      if (!new_buf) return NULL;
      memcpy(new_buf, buf, length);
      buf = new_buf;
    }
  }
  buf[length] = '\0';
  *out_len = length;
  return buf;
  /*#endregion*/
}

int main(int argc, char **argv) {
  /*#region*/
  setenv("MallocNanoZone", "0", 1);

  // 1. Initialize memory arena
  Arena *arena = arena_create(1024 * 1024);
  if (!arena) {
    fprintf(stderr, "Failed to initialize arena\n");
    return 1;
  }

  // 2. Read workflow definition from standard input
  size_t yaml_len = 0;
  char *yaml = read_stdin(arena, &yaml_len);
  if (!yaml || yaml_len == 0) {
    fprintf(stderr, "Error: No workflow definition provided via stdin.\n");
    arena_destroy(arena);
    return 1;
  }

  // 3. Parse and Compile the Workflow
  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, yaml_len, &ast);
  if (parse_status != ERR_SUCCESS) {
    fprintf(stderr, "Validation Error: Parser failed with code %d\n", parse_status);
    arena_destroy(arena);
    return 1;
  }

  int32_t compile_status = compile_workflow(arena, &ast);
  if (compile_status != ERR_SUCCESS) {
    fprintf(stderr, "Compilation Error: DAG Compiler failed with code %d\n", compile_status);
    arena_destroy(arena);
    return 1;
  }

  // 4. Setup Context Object
  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  // Parse command-line args for Inputs (key=value)
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  for (int i = 1; i < argc; i++) {
    char *eq = strchr(argv[i], '=');
    if (eq) {
      *eq = '\0';
      char *key = argv[i];
      char *val = eq + 1;
      jsonv_obj_set(jsonv_arena, inputs_obj, 
                    allocate_jsonv_string(arena, key), 
                    jsonv_val_str(allocate_jsonv_string(arena, val)));
      *eq = '='; // restore
    }
  }
  jsonv_obj_set(jsonv_arena, root_obj, 
                allocate_jsonv_string(arena, "inputs"), 
                jsonv_val_obj(inputs_obj));

  // Populate Secrets from environment variables starting with NESTOR_SECRET_
  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  for (char **env = environ; *env != NULL; env++) {
    char *env_entry = *env;
    if (strncmp(env_entry, "NESTOR_SECRET_", 14) == 0) {
      char *eq = strchr(env_entry, '=');
      if (eq) {
        *eq = '\0';
        char *key = env_entry + 14;
        char *val = eq + 1;
        jsonv_obj_set(jsonv_arena, secrets_obj, 
                      allocate_jsonv_string(arena, key), 
                      jsonv_val_str(allocate_jsonv_string(arena, val)));
        *eq = '='; // restore
      }
    }
  }
  jsonv_obj_set(jsonv_arena, root_obj, 
                allocate_jsonv_string(arena, "secrets"), 
                jsonv_val_obj(secrets_obj));

  // Populate Environment from host process
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  for (char **env = environ; *env != NULL; env++) {
    char *env_entry = *env;
    char *eq = strchr(env_entry, '=');
    if (eq) {
      *eq = '\0';
      char *key = env_entry;
      char *val = eq + 1;
      jsonv_obj_set(jsonv_arena, env_obj, 
                    allocate_jsonv_string(arena, key), 
                    jsonv_val_str(allocate_jsonv_string(arena, val)));
      *eq = '='; // restore
    }
  }
  jsonv_obj_set(jsonv_arena, root_obj, 
                allocate_jsonv_string(arena, "env"), 
                jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // 5. Run Workflow Execution Engine
  int32_t run_status = run_workflow(arena, &ast, &context_val);
  if (run_status != ERR_SUCCESS) {
    fprintf(stderr, "Runtime Error: Workflow execution failed with code %d\n", run_status);
    arena_destroy(arena);
    return 1;
  }

  // 6. Print steps outcomes in JSON format
  Jsonv_Value steps_val;
  if (jsonv_obj_get(root_obj, allocate_jsonv_string(arena, "steps"), &steps_val)) {
    // Convert outcomes to a readable string format
    Jsonv_Context *out_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
    if (out_ctx) {
      // Print the steps outcomes object
      printf("{\n  \"status\": \"success\",\n  \"steps\": {\n");
      int steps_len = jsonv_obj_length(steps_val.as.p);
      for (int i = 0; i < steps_len; i++) {
        const char *step_id = jsonv_obj_key_at(steps_val.as.p, i);
        Jsonv_Value outcome = jsonv_obj_val_at(steps_val.as.p, i);
        Jsonv_Value status_code_val;
        Jsonv_Value body_val;
        jsonv_obj_get(outcome.as.p, allocate_jsonv_string(arena, "status_code"), &status_code_val);
        jsonv_obj_get(outcome.as.p, allocate_jsonv_string(arena, "body"), &body_val);

        printf("    \"%.*s\": {\n      \"status_code\": %lld,\n", 
               (int)jsonv_val_str_len(jsonv_val_str(step_id)), step_id, status_code_val.as.i);
        
        printf("      \"body\": ");
        print_jsonv_value(body_val);
        printf("\n    }");

        if (i < steps_len - 1) {
          printf(",\n");
        } else {
          printf("\n");
        }
      }
      printf("  }\n}\n");
    }
  }

  arena_destroy(arena);
  return 0;
  /*#endregion*/
}
