#include "nestor_plugin.h"
#include "arena.h"
#include "evaluator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

void *na_alloc(Arena *arena, size_t size);

static char *allocate_jsonv_string(Arena *arena, const char *data, size_t len) {
  /*#region*/
  size_t total_size = sizeof(uint32_t) + len + 1;
  char *buf = na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)len;
  char *str_ptr = buf + sizeof(uint32_t);
  memcpy(str_ptr, data, len);
  str_ptr[len] = '\0';
  return str_ptr;
  /*#endregion*/
}

typedef struct {
  Arena *arena;
  Jsonv_Arena *jsonv_arena;
  Jsonv_Value context_val;
  Jsonv_Obj *outputs_obj;
} RunnerContext;

static void* runner_alloc(void *arena_ptr, size_t size) {
  /*#region*/
  return na_alloc((Arena *)arena_ptr, size);
  /*#endregion*/
}

static void runner_log(int level, const char *message) {
  /*#region*/
  const char *lvl = "INFO";
  if (level == NESTOR_LOG_DEBUG) lvl = "DEBUG";
  else if (level == NESTOR_LOG_WARN) lvl = "WARN";
  else if (level == NESTOR_LOG_ERROR) lvl = "ERROR";
  fprintf(stderr, "[SANDBOX:%s] %s\n", lvl, message);
  /*#endregion*/
}

static void* runner_jsonv_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void runner_jsonv_reset(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static void runner_jsonv_destroy(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static const Jsonv_Arena_Ops my_jsonv_ops = {
  .alloc = runner_jsonv_alloc,
  .reset = runner_jsonv_reset,
  .reset_to = NULL,
  .destroy = runner_jsonv_destroy
};

static const char* runner_get_variable(void *ctx_ptr, const char *json_path) {
  /*#region*/
  RunnerContext *rcx = (RunnerContext *)ctx_ptr;
  StringView expr = { json_path, strlen(json_path) };
  Jsonv_Value out_val = jsonv_val_undefined();
  
  int32_t rc = evaluate_expression(rcx->arena, expr, rcx->jsonv_arena, rcx->context_val, &out_val);

  if (rc == ERR_SUCCESS) {
    if (out_val.tag == JSONV_VAL_STRING) {
      return out_val.as.p;
    }
    char *serialized = NULL;
    if (serialize_jsonv_value(rcx->arena, out_val, &serialized) == ERR_SUCCESS) {
      return serialized;
    }
  }
  return NULL;
  /*#endregion*/
}

static int32_t runner_set_output(void *ctx_ptr, const char *key, const char *json_val) {
  /*#region*/
  RunnerContext *rcx = (RunnerContext *)ctx_ptr;
  Jsonv_Context *temp_ctx = jsonv_ctx_new(rcx->jsonv_arena, NULL, NULL);
  Jsonv_Value val = jsonv_val_undefined();
  if (temp_ctx && (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)json_val) ||
                   jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)json_val))) {
    jsonv_ctx_get_value(temp_ctx, &val);
  } else {
    char *str = allocate_jsonv_string(rcx->arena, json_val, strlen(json_val));
    val = jsonv_val_str(str);
  }

  char *k_key = allocate_jsonv_string(rcx->arena, key, strlen(key));
  jsonv_obj_set(rcx->jsonv_arena, rcx->outputs_obj, k_key, val);
  return 0;
  /*#endregion*/
}

int main(int argc, char **argv) {
  /*#region*/
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <plugin_path> <args_json>\n", argv[0]);
    return 1;
  }

  const char *plugin_path = argv[1];
  const char *args_json = argv[2];

  Arena *arena = arena_create(4 * 1024 * 1024);
  if (!arena) {
    fprintf(stderr, "Runner: failed to create arena\n");
    return 1;
  }

  // Load library
  char resolved_path[512];
  snprintf(resolved_path, sizeof(resolved_path), "%s", plugin_path);
  void *lib_handle = dlopen(resolved_path, RTLD_NOW | RTLD_LOCAL);
  if (!lib_handle) {
    snprintf(resolved_path, sizeof(resolved_path), "%s.so", plugin_path);
    lib_handle = dlopen(resolved_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib_handle) {
      snprintf(resolved_path, sizeof(resolved_path), "%s.dylib", plugin_path);
      lib_handle = dlopen(resolved_path, RTLD_NOW | RTLD_LOCAL);
    }
  }

  if (!lib_handle) {
    fprintf(stderr, "Runner error loading plugin '%s': %s\n", plugin_path, dlerror());
    arena_destroy(arena);
    return 1;
  }

  int32_t (*register_fn)(const NestorHostAPI*, NestorPluginAPI*) = 
    dlsym(lib_handle, "nestor_plugin_register");
  if (!register_fn) {
    fprintf(stderr, "Runner error: entry symbol 'nestor_plugin_register' not found\n");
    dlclose(lib_handle);
    arena_destroy(arena);
    return 1;
  }

  NestorHostAPI host_api = {
    .alloc = runner_alloc,
    .log = runner_log,
    .get_variable = runner_get_variable,
    .set_output = runner_set_output
  };

  NestorPluginAPI plugin_api;
  memset(&plugin_api, 0, sizeof(plugin_api));
  int32_t rc = register_fn(&host_api, &plugin_api);
  if (rc != 0) {
    fprintf(stderr, "Runner: plugin registration failed with code %d\n", rc);
    dlclose(lib_handle);
    arena_destroy(arena);
    return 1;
  }

  if (plugin_api.init) {
    rc = plugin_api.init(&host_api);
    if (rc != 0) {
      fprintf(stderr, "Runner: plugin init failed with code %d\n", rc);
      if (plugin_api.shutdown) plugin_api.shutdown();
      dlclose(lib_handle);
      arena_destroy(arena);
      return 1;
    }
  }

  // Prepare runner context evaluator
  Jsonv_Arena *jsonv_arena = jsonv_arena_new_custom(&my_jsonv_ops, arena);
  (void)jsonv_ctx_new(jsonv_arena, NULL, NULL);

  // Load NESTOR_CONTEXT if present
  Jsonv_Value context_val = jsonv_val_undefined();
  const char *ctx_json_str = getenv("NESTOR_CONTEXT");
  if (ctx_json_str) {
    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
    if (temp_ctx) {
      if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)ctx_json_str) ||
          jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)ctx_json_str)) {
        jsonv_ctx_get_value(temp_ctx, &context_val);
      }
    }
  }
  if (context_val.tag != JSONV_VAL_OBJ) {
    context_val = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
  }

  RunnerContext runner_ctx = {
    .arena = arena,
    .jsonv_arena = jsonv_arena,
    .context_val = context_val,
    .outputs_obj = jsonv_obj_new(jsonv_arena, NULL)
  };

  int32_t exec_rc = 0;
  if (plugin_api.execute) {
    exec_rc = plugin_api.execute(arena, &runner_ctx, args_json);
  }

  if (plugin_api.shutdown) {
    plugin_api.shutdown();
  }
  dlclose(lib_handle);

  // Serialize outputs to print to stdout
  char *outputs_str = NULL;
  serialize_jsonv_value(arena, jsonv_val_obj(runner_ctx.outputs_obj), &outputs_str);
  if (!outputs_str) {
    outputs_str = "{}";
  }

  printf("{\"status_code\": %d, \"outputs\": %s}\n", exec_rc, outputs_str);

  jsonv_arena_destroy(jsonv_arena);
  arena_destroy(arena);
  return exec_rc;
  /*#endregion*/
}
