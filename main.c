#include "nestor.h"
#include "parser.h"
#include "cache.h"
#include "aho_corasick.h"
#include "loader.h"
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

void print_jsonv_value(Jsonv_Value v) {
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
    case JSONV_VAL_STRING: {
      size_t str_len = jsonv_val_str_len(v);
      const char *str = (const char *)v.as.p;
      putchar('"');
      for (size_t k = 0; k < str_len; k++) {
        char c = str[k];
        if (c == '"') fputs("\\\"", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else putchar(c);
      }
      putchar('"');
      break;
    }
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

  // Initialize Cache
  if (cache_init(".nestor_cache.db", 1000) != ERR_SUCCESS) {
    fprintf(stderr, "Warning: Failed to initialize SQLite cache database\n");
  }

  // Check subcommands: plan, compile, apply
  if (argc > 1) {
    if (strcmp(argv[1], "plan") == 0) {
      const char *proj_dir = argc > 2 ? argv[2] : ".";
      WorkspaceMap map;
      int32_t load_res = workspace_load_directory(arena, proj_dir, &map);
      if (load_res != ERR_SUCCESS) {
        fprintf(stderr, "ERR_INVALID_CONTRACT: Plan failed with code %d\n", load_res);
        arena_destroy(arena);
        return 1;
      }
      printf("Plan succeeded: Loaded %zu workflows, %zu providers. Zero contract or dependency cycles detected.\n",
             map.workflow_count, map.provider_count);
      arena_destroy(arena);
      return 0;
    } else if (strcmp(argv[1], "compile") == 0) {
      const char *proj_dir = argc > 2 ? argv[2] : ".";
      const char *out_path = "output.nbc";
      for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-o") == 0) {
          out_path = argv[i + 1];
        }
      }
      WorkspaceMap map;
      int32_t load_res = workspace_load_directory(arena, proj_dir, &map);
      if (load_res != ERR_SUCCESS) {
        fprintf(stderr, "Compile error loading workspace: %d\n", load_res);
        arena_destroy(arena);
        return 1;
      }
      int32_t bc_res = bytecode_compile_workspace(arena, &map, out_path);
      if (bc_res != ERR_SUCCESS) {
        fprintf(stderr, "Bytecode compile failed with code %d\n", bc_res);
        arena_destroy(arena);
        return 1;
      }
      printf("Compiled workspace to %s successfully.\n", out_path);
      arena_destroy(arena);
      return 0;
    } else if (strcmp(argv[1], "apply") == 0) {
      if (argc > 2 && strstr(argv[2], ".nbc")) {
        Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(NULL);
        NVMContext nvm_ctx;
        int32_t init_res = nvm_init_from_file(&nvm_ctx, arena, jsonv_arena, argv[2]);
        if (init_res != ERR_SUCCESS) {
          fprintf(stderr, "Failed to map NBC file %s: %d\n", argv[2], init_res);
          arena_destroy(arena);
          return 1;
        }
        int32_t exec_res = nvm_execute_loop(&nvm_ctx);
        nvm_close(&nvm_ctx);
        if (exec_res != ERR_SUCCESS) {
          fprintf(stderr, "NVM execution error %d\n", exec_res);
          arena_destroy(arena);
          return 1;
        }
        printf("NVM Execution Completed Successfully.\n");
        arena_destroy(arena);
        return 0;
      }
    }
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

  // Load workspace map to resolve declarative providers/workflows if they exist
  WorkspaceMap *map = na_alloc(arena, sizeof(WorkspaceMap));
  if (map) {
    memset(map, 0, sizeof(WorkspaceMap));
    workspace_load_directory(arena, ".", map);
    ast.providers_map = map;
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
    cache_close();
    arena_destroy(arena);
    return 1;
  }

  // 6. Print clean execution context outcomes (outputs if present and not in debug mode, else jobs & status)
  bool debug_mode = false;
  const char *env_debug = getenv("NESTOR_DEBUG");
  if (env_debug && (strcmp(env_debug, "1") == 0 || strcmp(env_debug, "true") == 0)) {
    debug_mode = true;
  }
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--verbose") == 0) {
      debug_mode = true;
    }
  }

  Jsonv_Value to_serialize = jsonv_val_undefined();
  Jsonv_Value outputs_val;
  bool has_outputs = jsonv_obj_get(root_obj, allocate_jsonv_string(arena, "outputs"), &outputs_val);

  if (has_outputs && !debug_mode) {
    to_serialize = outputs_val;
  } else {
    Jsonv_Obj *clean_obj = jsonv_obj_new(jsonv_arena, NULL);
    Jsonv_Value jobs_val;
    if (jsonv_obj_get(root_obj, allocate_jsonv_string(arena, "jobs"), &jobs_val)) {
      jsonv_obj_set(jsonv_arena, clean_obj, allocate_jsonv_string(arena, "jobs"), jobs_val);
    }
    if (has_outputs) {
      jsonv_obj_set(jsonv_arena, clean_obj, allocate_jsonv_string(arena, "outputs"), outputs_val);
    }
    jsonv_obj_set(jsonv_arena, clean_obj, allocate_jsonv_string(arena, "status"), jsonv_val_str(allocate_jsonv_string(arena, "success")));
    to_serialize = jsonv_val_obj(clean_obj);
  }

  char *json_out = NULL;
  if (serialize_jsonv_value(arena, to_serialize, &json_out) == ERR_SUCCESS) {
    ACNode *ac_root = ac_create_trie(arena, context_val);
    if (ac_root) {
      char *redacted_json = na_alloc(arena, strlen(json_out) * 2 + 1);
      if (redacted_json) {
        redact_stream(ac_root, json_out, redacted_json, strlen(json_out));
        printf("%s\n", redacted_json);
      } else {
        printf("%s\n", json_out);
      }
    } else {
      printf("%s\n", json_out);
    }
  }

  cache_close();
  arena_destroy(arena);
  return 0;
  /*#endregion*/
}
