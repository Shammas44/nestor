#include "plugin.h"
#include "evaluator.h"
#include "aho_corasick.h"
#include "stringview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

void *na_alloc(Arena *arena, size_t size);


int32_t plugin_start(PluginExecutor *pe, Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value context_val, StepNode *step) {
  /*#region*/
  Jsonv_Value resolved_with = resolve_json_value(arena, step->plugin.with_args, jsonv_arena, context_val);
  
  char *temp_in_path = na_alloc(arena, 256);
  if (!temp_in_path) return ERR_OOM;
  snprintf(temp_in_path, 256, "/tmp/nestor_input_%p.json", (void *)step);
  FILE *f_in = fopen(temp_in_path, "w");
  if (!f_in) return ERR_HTTP_TRANSPORT;
  char *in_json_str = "";
  if (serialize_jsonv_value(arena, resolved_with, &in_json_str) == ERR_SUCCESS) {
    fputs(in_json_str, f_in);
  }
  fclose(f_in);

  Jsonv_Value env_val;
  char *env_json_str = "";
  if (jsonv_obj_get(context_val.as.p, "env", &env_val)) {
    serialize_jsonv_value(arena, env_val, &env_json_str);
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
    if (fp) {
      fclose(fp);
    } else {
      snprintf(plugin_path, sizeof(plugin_path), "/usr/local/share/nestor/plugins/%s", uses_cstr);
    }
  }

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
    if (ast->ipc_socket_path[0] != '\0') {
      setenv("NESTOR_SOCKET", ast->ipc_socket_path, 1);
    }

    char *args[] = { plugin_path, NULL };
    execvp(plugin_path, args);
    exit(127);
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
