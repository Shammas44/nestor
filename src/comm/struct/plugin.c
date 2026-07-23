#include "plugin.h"
#include "evaluator.h"
#include "aho_corasick.h"
#include "stringview.h"
#include "nestor_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
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
} HostContext;

static void* host_alloc(void *arena_ptr, size_t size) {
  /*#region*/
  return na_alloc((Arena *)arena_ptr, size);
  /*#endregion*/
}

static void host_log(int level, const char *message) {
  /*#region*/
  const char *lvl = "INFO";
  if (level == NESTOR_LOG_DEBUG) lvl = "DEBUG";
  else if (level == NESTOR_LOG_WARN) lvl = "WARN";
  else if (level == NESTOR_LOG_ERROR) lvl = "ERROR";
  fprintf(stderr, "[PLUGIN:%s] %s\n", lvl, message);
  /*#endregion*/
}

static const char* host_get_variable(void *ctx_ptr, const char *json_path) {
  /*#region*/
  HostContext *hc = (HostContext *)ctx_ptr;
  StringView expr = { json_path, strlen(json_path) };
  Jsonv_Value out_val = jsonv_val_undefined();
  int32_t rc = evaluate_expression(hc->arena, expr, hc->jsonv_arena, hc->context_val, &out_val);
  if (rc == ERR_SUCCESS) {
    if (out_val.tag == JSONV_VAL_STRING) {
      return out_val.as.p;
    }
    char *serialized = NULL;
    if (serialize_jsonv_value(hc->arena, out_val, &serialized) == ERR_SUCCESS) {
      return serialized;
    }
  }
  return NULL;
  /*#endregion*/
}

static int32_t host_set_output(void *ctx_ptr, const char *key, const char *json_val) {
  /*#region*/
  HostContext *hc = (HostContext *)ctx_ptr;
  Jsonv_Context *temp_ctx = jsonv_ctx_new(hc->jsonv_arena, NULL, NULL);
  Jsonv_Value val = jsonv_val_undefined();
  if (temp_ctx && (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)json_val) ||
                   jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)json_val))) {
    jsonv_ctx_get_value(temp_ctx, &val);
  } else {
    char *str = allocate_jsonv_string(hc->arena, json_val, strlen(json_val));
    val = jsonv_val_str(str);
  }

  char *k_key = allocate_jsonv_string(hc->arena, key, strlen(key));
  jsonv_obj_set(hc->jsonv_arena, hc->outputs_obj, k_key, val);
  return 0;
  /*#endregion*/
}

int32_t plugin_start(PluginExecutor *pe, Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value context_val, StepNode *step) {
  /*#region*/
  Jsonv_Value resolved_with = resolve_json_value(arena, step->plugin.with_args, jsonv_arena, context_val);
  char *in_json_str = "";
  if (serialize_jsonv_value(arena, resolved_with, &in_json_str) != ERR_SUCCESS) {
    in_json_str = "{}";
  }

  char plugin_path[512];
  StringView uses_sv = step->plugin.uses;
  char *uses_cstr = sv_to_cstring(arena, uses_sv);
  if (uses_cstr[0] == '/' || (uses_cstr[0] == '.' && uses_cstr[1] == '/')) {
    snprintf(plugin_path, sizeof(plugin_path), "%s", uses_cstr);
  } else {
    char *at_char = strchr(uses_cstr, '@');
    if (at_char) *at_char = '\0';
    snprintf(plugin_path, sizeof(plugin_path), "./plugins/%s", uses_cstr);
    FILE *fp = fopen(plugin_path, "r");
    if (!fp) {
      char tmp_path[540];
      snprintf(tmp_path, sizeof(tmp_path), "%s.so", plugin_path);
      fp = fopen(tmp_path, "r");
      if (!fp) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.dylib", plugin_path);
        fp = fopen(tmp_path, "r");
      }
    }
    if (fp) {
      fclose(fp);
    } else {
      snprintf(plugin_path, sizeof(plugin_path), "/usr/local/share/nestor/plugins/%s", uses_cstr);
    }
  }

  // In-process dynamic loading (trusted dynlib)
  if (!step->plugin.sandboxed) {
    void *lib_handle = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib_handle) {
      char path_with_ext[540];
      snprintf(path_with_ext, sizeof(path_with_ext), "%s.so", plugin_path);
      lib_handle = dlopen(path_with_ext, RTLD_NOW | RTLD_LOCAL);
      if (!lib_handle) {
        snprintf(path_with_ext, sizeof(path_with_ext), "%s.dylib", plugin_path);
        lib_handle = dlopen(path_with_ext, RTLD_NOW | RTLD_LOCAL);
      }
      if (lib_handle) {
        strcpy(plugin_path, path_with_ext);
      }
    }

    if (lib_handle) {
      int32_t (*register_fn)(const NestorHostAPI*, NestorPluginAPI*) = 
        dlsym(lib_handle, "nestor_plugin_register");
      if (register_fn) {
        NestorHostAPI host_api = {
          .alloc = host_alloc,
          .log = host_log,
          .get_variable = host_get_variable,
          .set_output = host_set_output
        };

        NestorPluginAPI plugin_api;
        memset(&plugin_api, 0, sizeof(plugin_api));
        int32_t rc = register_fn(&host_api, &plugin_api);
        if (rc == 0) {
          if (plugin_api.init) {
            plugin_api.init(&host_api);
          }

          HostContext host_ctx = {
            .arena = arena,
            .jsonv_arena = jsonv_arena,
            .context_val = context_val,
            .outputs_obj = jsonv_obj_new(jsonv_arena, NULL)
          };

          int32_t exec_rc = 0;
          if (plugin_api.execute) {
            exec_rc = plugin_api.execute(arena, &host_ctx, in_json_str);
          }

          if (plugin_api.shutdown) {
            plugin_api.shutdown();
          }
          dlclose(lib_handle);

          // Serialize output to response buffer
          char *outputs_str = NULL;
          serialize_jsonv_value(arena, jsonv_val_obj(host_ctx.outputs_obj), &outputs_str);
          if (!outputs_str) {
            outputs_str = "{}";
          }

          size_t needed_cap = strlen(outputs_str) + 128;
          pe->child_resp_buf.arena = arena;
          pe->child_resp_buf.buf = na_alloc(arena, needed_cap);
          if (pe->child_resp_buf.buf) {
            pe->child_resp_buf.cap = needed_cap;
            sprintf(pe->child_resp_buf.buf, "{\"status_code\": %d, \"outputs\": %s}", exec_rc, outputs_str);
            pe->child_resp_buf.len = strlen(pe->child_resp_buf.buf);
          }

          pe->child_stderr_buf.arena = arena;
          pe->child_stderr_buf.buf = na_alloc(arena, 16);
          if (pe->child_stderr_buf.buf) {
            pe->child_stderr_buf.cap = 16;
            pe->child_stderr_buf.buf[0] = '\0';
            pe->child_stderr_buf.len = 0;
          }

          pe->child_pid = -1;
          pe->in_process_exit_code = exec_rc;
          pe->temp_in_path = NULL;
          return ERR_SUCCESS;
        }
      }
      dlclose(lib_handle);
    }
  }

  // Create temporary input file for subprocess
  char *temp_in_path = na_alloc(arena, 256);
  if (!temp_in_path) return ERR_OOM;
  snprintf(temp_in_path, 256, "/tmp/nestor_input_%p.json", (void *)step);
  FILE *f_in = fopen(temp_in_path, "w");
  if (!f_in) return ERR_HTTP_TRANSPORT;
  fputs(in_json_str, f_in);
  fclose(f_in);

  Jsonv_Value env_val;
  char *env_json_str = "";
  if (jsonv_obj_get(context_val.as.p, "env", &env_val)) {
    serialize_jsonv_value(arena, env_val, &env_json_str);
  }

  char *ctx_json_str = "";
  serialize_jsonv_value(arena, context_val, &ctx_json_str);

  int stdout_pipe[2];
  int stderr_pipe[2];
  if (pipe(stdout_pipe) == -1) {
    unlink(temp_in_path);
    return ERR_HTTP_TRANSPORT;
  }
  if (pipe(stderr_pipe) == -1) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    unlink(temp_in_path);
    return ERR_HTTP_TRANSPORT;
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    unlink(temp_in_path);
    return ERR_HTTP_TRANSPORT;
  }

  if (pid == 0) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    setenv("NESTOR_PLUGIN_INPUT", temp_in_path, 1);
    setenv("NESTOR_ENV", env_json_str, 1);
    setenv("NESTOR_CONTEXT", ctx_json_str, 1);
    if (ast->ipc_socket_path[0] != '\0') {
      setenv("NESTOR_SOCKET", ast->ipc_socket_path, 1);
    }

    if (step->plugin.sandboxed) {
      char runner_path[512] = "./bin/nestor-plugin-runner";
      char *args[] = { runner_path, plugin_path, in_json_str, NULL };
      execvp(runner_path, args);
      exit(127);
    } else {
      char *args[] = { plugin_path, NULL };
      execvp(plugin_path, args);
      exit(127);
    }
  }

  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
  fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);

  int err_flags = fcntl(stderr_pipe[0], F_GETFL, 0);
  fcntl(stderr_pipe[0], F_SETFL, err_flags | O_NONBLOCK);

  pe->child_pid = pid;
  pe->child_stdout_fd = stdout_pipe[0];
  pe->child_stderr_fd = stderr_pipe[0];
  pe->temp_in_path = temp_in_path;

  // Initialize response buffers
  pe->child_resp_buf.arena = arena;
  pe->child_resp_buf.len = 0;
  pe->child_resp_buf.cap = 4096;
  pe->child_resp_buf.buf = na_alloc(arena, pe->child_resp_buf.cap);
  if (pe->child_resp_buf.buf) {
    pe->child_resp_buf.buf[0] = '\0';
  }

  pe->child_stderr_buf.arena = arena;
  pe->child_stderr_buf.len = 0;
  pe->child_stderr_buf.cap = 4096;
  pe->child_stderr_buf.buf = na_alloc(arena, pe->child_stderr_buf.cap);
  if (pe->child_stderr_buf.buf) {
    pe->child_stderr_buf.buf[0] = '\0';
  }

  // Timeout parsing
  pe->child_timeout_ms = 30000;
  if (step->timeout.length > 0) {
    StringView resolved_timeout;
    resolve_string(arena, step->timeout, jsonv_arena, context_val, &resolved_timeout);
    pe->child_timeout_ms = parse_duration_ms(resolved_timeout);
  }

  gettimeofday(&pe->child_start_time, NULL);
  return ERR_SUCCESS;
  /*#endregion*/
}

static void log_stderr_redacted(Arena *arena, StepNode *step, const char *buf, ssize_t len) {
  /*#region*/
  ACNode *ac_root = get_global_ac_root();
  if (ac_root) {
    char *redacted = na_alloc(arena, len * 2 + 1);
    if (redacted) {
      size_t red_len = redact_stream(ac_root, buf, redacted, len);
      fprintf(stderr, "[%*.*s:stderr] %.*s", 
              (int)step->id.length, (int)step->id.length, step->id.data,
              (int)red_len, redacted);
      return;
    }
  }
  fprintf(stderr, "[%*.*s:stderr] %.*s", 
          (int)step->id.length, (int)step->id.length, step->id.data,
          (int)len, buf);
  /*#endregion*/
}

int32_t plugin_poll(PluginExecutor *pe, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, StepNode *step, bool *finished, long *exit_code) {
  /*#region*/
  (void)jsonv_arena;
  (void)context_val;
  if (pe->child_pid == -1) {
    *finished = true;
    *exit_code = pe->in_process_exit_code;
    return ERR_SUCCESS;
  }
  *finished = false;
  
  // Read stdout
  char read_buf[512];
  ssize_t n;
  while ((n = read(pe->child_stdout_fd, read_buf, sizeof(read_buf))) > 0) {
    if (pe->child_resp_buf.len + n >= pe->child_resp_buf.cap) {
      pe->child_resp_buf.cap = pe->child_resp_buf.len + n + 4096;
      char *new_buf = na_alloc(arena, pe->child_resp_buf.cap);
      if (!new_buf) break;
      memcpy(new_buf, pe->child_resp_buf.buf, pe->child_resp_buf.len);
      pe->child_resp_buf.buf = new_buf;
    }
    memcpy(pe->child_resp_buf.buf + pe->child_resp_buf.len, read_buf, n);
    pe->child_resp_buf.len += n;
    pe->child_resp_buf.buf[pe->child_resp_buf.len] = '\0';
  }

  // Read stderr
  char err_buf[512];
  ssize_t n_err;
  while ((n_err = read(pe->child_stderr_fd, err_buf, sizeof(err_buf))) > 0) {
    if (pe->child_stderr_buf.len + n_err >= pe->child_stderr_buf.cap) {
      pe->child_stderr_buf.cap = pe->child_stderr_buf.len + n_err + 4096;
      char *new_buf = na_alloc(arena, pe->child_stderr_buf.cap);
      if (!new_buf) break;
      memcpy(new_buf, pe->child_stderr_buf.buf, pe->child_stderr_buf.len);
      pe->child_stderr_buf.buf = new_buf;
    }
    memcpy(pe->child_stderr_buf.buf + pe->child_stderr_buf.len, err_buf, n_err);
    pe->child_stderr_buf.len += n_err;
    pe->child_stderr_buf.buf[pe->child_stderr_buf.len] = '\0';
    log_stderr_redacted(arena, step, err_buf, n_err);
  }

  // Check timeout
  struct timeval now;
  gettimeofday(&now, NULL);
  long elapsed_ms = (now.tv_sec - pe->child_start_time.tv_sec) * 1000 + 
                    (now.tv_usec - pe->child_start_time.tv_usec) / 1000;
  if (elapsed_ms > pe->child_timeout_ms) {
    fprintf(stderr, "PLUGIN TIMEOUT: step %.*s exceeded %ld ms\n", 
            (int)step->id.length, step->id.data, pe->child_timeout_ms);
    kill(pe->child_pid, SIGKILL);
    int status = 0;
    waitpid(pe->child_pid, &status, 0);
    
    close(pe->child_stdout_fd);
    close(pe->child_stderr_fd);
    
    *finished = true;
    *exit_code = -4; // ERR_TIMEOUT
    return ERR_SUCCESS;
  }

  // Check exit status
  int status = 0;
  pid_t res = waitpid(pe->child_pid, &status, WNOHANG);
  if (res == pe->child_pid) {
    // Read leftover stdout
    while ((n = read(pe->child_stdout_fd, read_buf, sizeof(read_buf))) > 0) {
      if (pe->child_resp_buf.len + n >= pe->child_resp_buf.cap) {
        pe->child_resp_buf.cap = pe->child_resp_buf.len + n + 4096;
        char *new_buf = na_alloc(arena, pe->child_resp_buf.cap);
        if (!new_buf) break;
        memcpy(new_buf, pe->child_resp_buf.buf, pe->child_resp_buf.len);
        pe->child_resp_buf.buf = new_buf;
      }
      memcpy(pe->child_resp_buf.buf + pe->child_resp_buf.len, read_buf, n);
      pe->child_resp_buf.len += n;
      pe->child_resp_buf.buf[pe->child_resp_buf.len] = '\0';
    }

    // Read leftover stderr
    while ((n_err = read(pe->child_stderr_fd, err_buf, sizeof(err_buf))) > 0) {
      if (pe->child_stderr_buf.len + n_err >= pe->child_stderr_buf.cap) {
        pe->child_stderr_buf.cap = pe->child_stderr_buf.len + n_err + 4096;
        char *new_buf = na_alloc(arena, pe->child_stderr_buf.cap);
        if (!new_buf) break;
        memcpy(new_buf, pe->child_stderr_buf.buf, pe->child_stderr_buf.len);
        pe->child_stderr_buf.buf = new_buf;
      }
      memcpy(pe->child_stderr_buf.buf + pe->child_stderr_buf.len, err_buf, n_err);
      pe->child_stderr_buf.len += n_err;
      pe->child_stderr_buf.buf[pe->child_stderr_buf.len] = '\0';
      log_stderr_redacted(arena, step, err_buf, n_err);
    }

    close(pe->child_stdout_fd);
    close(pe->child_stderr_fd);

    *finished = true;
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

void plugin_cleanup(PluginExecutor *pe) {
  /*#region*/
  if (pe->temp_in_path) {
    unlink(pe->temp_in_path);
    pe->temp_in_path = NULL;
  }
  pe->child_pid = 0;
  pe->child_stdout_fd = -1;
  pe->child_stderr_fd = -1;
  /*#endregion*/
}
