#include "runner.h"
#include "loader.h"
#include "evaluator.h"
#include <jsonata/jsonata.h>
#include "aho_corasick.h"
#include "transport.h"
#include "plugin.h"
#include "cache.h"
#include <time.h>
#include <curl/curl.h>
#include "parser.h"
#include "compiler.h"

#ifdef jsonv_val_obj
#undef jsonv_val_obj
#endif
#define jsonv_val_obj(x) ((Jsonv_Value){.tag = JSONV_VAL_OBJ, .as = {.p = (x)}})

#ifdef jsonv_val_arr
#undef jsonv_val_arr
#endif
#define jsonv_val_arr(x) ((Jsonv_Value){.tag = JSONV_VAL_ARRAY, .as = {.p = (x)}})

#ifdef jsonv_val_undefined
#undef jsonv_val_undefined
#endif
#define jsonv_val_undefined() ((Jsonv_Value){.tag = JSONV_VAL_UNDEFINED, .as = {.p = NULL}})

typedef struct ActiveJob ActiveJob;
static void store_job_in_cache(Arena *arena, WorkflowAST *ast, Jsonv_Value context_val, JobNode *job, ActiveJob *aj);
static bool check_and_apply_cache(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, JobNode *job, ActiveJob *aj_out);
static int32_t advance_active_job(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, ActiveJob *aj, Transport *transport);

static void *my_jsonv_arena_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void my_jsonv_arena_reset(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static void my_jsonv_arena_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  (void)user_data;
  (void)keep_size;
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

void *na_alloc(Arena *arena, size_t size);

static void push_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars);
static void pop_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars);
static int32_t evaluate_job_variables(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, JobNode *job, Jsonv_Value local_vars);
static int32_t evaluate_step_variables(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, JobNode *job, StepNode *step, Jsonv_Value local_vars);



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
}

static size_t my_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  /*#region*/
  size_t realsize = size * nmemb;
  ResponseBuffer *mem = (ResponseBuffer *)userp;
  if (mem->len + realsize >= mem->cap) {
    mem->cap = mem->len + realsize + 4096;
    char *new_buf = na_alloc(mem->arena, mem->cap);
    if (!new_buf) return 0;
    memcpy(new_buf, mem->buf, mem->len);
    mem->buf = new_buf;
  }
  memcpy(mem->buf + mem->len, contents, realsize);
  mem->len += realsize;
  mem->buf[mem->len] = '\0';
  return realsize;
  /*#endregion*/
}




int32_t execute_step(Arena *arena, StepNode *step, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value steps_state_obj) {
  /*#region*/
  if (step->is_http) {
    // 1. Resolve URL and method
    StringView resolved_url;
    int32_t status = resolve_string(arena, step->http.url, jsonv_arena, context_val, &resolved_url);
    if (status != ERR_SUCCESS) return status;

    StringView resolved_method;
    status = resolve_string(arena, step->http.method, jsonv_arena, context_val, &resolved_method);
    if (status != ERR_SUCCESS) return status;

    // Convert to null-terminated C-strings
    char *url_cstr = na_alloc(arena, resolved_url.length + 1);
    memcpy(url_cstr, resolved_url.data, resolved_url.length);
    url_cstr[resolved_url.length] = '\0';

    char *method_cstr = na_alloc(arena, resolved_method.length + 1);
    memcpy(method_cstr, resolved_method.data, resolved_method.length);
    method_cstr[resolved_method.length] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) return ERR_HTTP_TRANSPORT;

    // 2. Setup URL and method
    curl_easy_setopt(curl, CURLOPT_URL, url_cstr);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (strcmp(method_cstr, "POST") == 0) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      if (step->http.body.tag == JSONV_VAL_UNDEFINED) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
      }
    } else if (strcmp(method_cstr, "GET") != 0) {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method_cstr);
      if (step->http.body.tag == JSONV_VAL_UNDEFINED &&
          (strcmp(method_cstr, "PUT") == 0 || strcmp(method_cstr, "PATCH") == 0)) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
      }
    }

    // 3. Resolve and set headers
    struct curl_slist *header_list = NULL;
    if (step->http.headers.tag == JSONV_VAL_OBJ) {
      Jsonv_Obj *headers_obj = step->http.headers.as.p;
      int len = jsonv_obj_length(headers_obj);
      for (int k = 0; k < len; k++) {
        const char *key = jsonv_obj_key_at(headers_obj, k);
        Jsonv_Value val = jsonv_obj_val_at(headers_obj, k);
        
        StringView resolved_val;
        if (val.tag == JSONV_VAL_STRING) {
          StringView orig_val = { (const char *)val.as.p, jsonv_val_str_len(val) };
          status = resolve_string(arena, orig_val, jsonv_arena, context_val, &resolved_val);
          if (status != ERR_SUCCESS) {
            curl_easy_cleanup(curl);
            return status;
          }
        } else {
          resolved_val = (StringView){"", 0};
        }

        // Format: Key: Value
        size_t hdr_cap = strlen(key) + resolved_val.length + 4;
        char *hdr_buf = na_alloc(arena, hdr_cap);
        snprintf(hdr_buf, hdr_cap, "%s: %.*s", key, (int)resolved_val.length, resolved_val.data);
        header_list = curl_slist_append(header_list, hdr_buf);
      }
    }
    if (header_list) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // 4. Resolve and set body
    if (step->http.body.tag != JSONV_VAL_UNDEFINED) {
      Jsonv_Value resolved_body = resolve_json_value(arena, step->http.body, jsonv_arena, context_val);
      char *body_cstr = "";
      if (resolved_body.tag == JSONV_VAL_STRING) {
        body_cstr = (char *)resolved_body.as.p;
      } else {
        serialize_jsonv_value(arena, resolved_body, &body_cstr);
      }
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_cstr);
    }

    // 5. Parse timeout
    long timeout_ms = 30000;
    if (step->timeout.length > 0) {
      StringView resolved_timeout;
      resolve_string(arena, step->timeout, jsonv_arena, context_val, &resolved_timeout);
      long val = 0;
      size_t idx = 0;
      while (idx < resolved_timeout.length && resolved_timeout.data[idx] >= '0' && resolved_timeout.data[idx] <= '9') {
        val = val * 10 + (resolved_timeout.data[idx] - '0');
        idx++;
      }
      StringView unit = { resolved_timeout.data + idx, resolved_timeout.length - idx };
      if (sv_equals_cstr(unit, "ms")) {
        timeout_ms = val;
      } else if (sv_equals_cstr(unit, "s") || unit.length == 0) {
        timeout_ms = val * 1000;
      } else if (sv_equals_cstr(unit, "m")) {
        timeout_ms = val * 60 * 1000;
      }
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

    ResponseBuffer resp_buf = { .arena = arena };
    resp_buf.cap = 4096;
    resp_buf.buf = na_alloc(arena, resp_buf.cap);
    resp_buf.buf[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp_buf);

    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;
    if (res == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    } else {
      if (header_list) curl_slist_free_all(header_list);
      curl_easy_cleanup(curl);
      return ERR_HTTP_TRANSPORT;
    }

    // 7. Parse response body into Jsonv_Value
    Jsonv_Value body_val = jsonv_val_undefined();
    if (resp_buf.len > 0) {
      Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
      if (temp_ctx) {
        if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)resp_buf.buf) ||
            jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)resp_buf.buf)) {
          jsonv_ctx_get_value(temp_ctx, &body_val);
        } else {
          char *str = allocate_jsonv_string(arena, resp_buf.buf, resp_buf.len);
          body_val = jsonv_val_str(str);
        }
      } else {
        char *str = allocate_jsonv_string(arena, resp_buf.buf, resp_buf.len);
        body_val = jsonv_val_str(str);
      }
    } else {
      body_val = jsonv_val_null();
    }

    // 8. Save output
    Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
    char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
    char *k_body = allocate_jsonv_string(arena, "body", 4);
    jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(status_code));
    jsonv_obj_set(jsonv_arena, outcome_obj, k_body, body_val);

    char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
    jsonv_obj_set(jsonv_arena, steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

  } else {
    // PLUGIN Step execution
    Jsonv_Value resolved_with = resolve_json_value(arena, step->plugin.with_args, jsonv_arena, context_val);
    
    char temp_in_path[256];
    snprintf(temp_in_path, sizeof(temp_in_path), "/tmp/nestor_input_%p.json", (void *)step);
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

    if (strncmp(uses_cstr, "workflows.", 10) == 0) {
      char sub_path[512];
      snprintf(sub_path, sizeof(sub_path), "workflows/%s.yaml", uses_cstr + 10);
      FILE *sf = fopen(sub_path, "r");
      if (!sf) {
        snprintf(sub_path, sizeof(sub_path), "workflows/%s.yml", uses_cstr + 10);
        sf = fopen(sub_path, "r");
      }
      if (sf) {
        fclose(sf);
        WorkflowAST sub_ast;
        int32_t parse_status = parser_parse_file(arena, sub_path, &sub_ast);
        if (parse_status == ERR_SUCCESS) {
          int32_t compile_status = compile_workflow(arena, &sub_ast);
          if (compile_status == ERR_SUCCESS) {
            Jsonv_Arena *sub_jarena = jsonv_ctx_arena(sub_ast.jsonv_ctx);
            Jsonv_Obj *sub_root_obj = jsonv_obj_new(sub_jarena, NULL);
            jsonv_obj_set(sub_jarena, sub_root_obj, allocate_jsonv_string(arena, "inputs", 6), resolved_with);
            Jsonv_Value sub_context = jsonv_val_obj(sub_root_obj);

            int32_t run_res = run_workflow(arena, &sub_ast, &sub_context);
            long status_code = (run_res == ERR_SUCCESS) ? 0 : 500;
            Jsonv_Value body_val = jsonv_val_undefined();
            if (run_res == ERR_SUCCESS) {
              Jsonv_Value outputs_val;
              if (jsonv_obj_get(sub_root_obj, "outputs", &outputs_val)) {
                body_val = outputs_val;
              }
            }
            Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
            char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
            char *k_body = allocate_jsonv_string(arena, "body", 4);
            jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(status_code));
            jsonv_obj_set(jsonv_arena, outcome_obj, k_body, body_val);

            char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
            jsonv_obj_set(jsonv_arena, steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));
            return run_res;
          }
        }
      }
    }

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

    int pipefd[2];
    if (pipe(pipefd) == -1) {
      unlink(temp_in_path);
      return ERR_HTTP_TRANSPORT;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipefd[0]);
      close(pipefd[1]);
      unlink(temp_in_path);
      return ERR_HTTP_TRANSPORT;
    }

    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);

      setenv("NESTOR_PLUGIN_INPUT", temp_in_path, 1);
      setenv("NESTOR_ENV", env_json_str, 1);

      char *args[] = { plugin_path, NULL };
      execvp(plugin_path, args);
      exit(127);
    } else {
      close(pipefd[1]);

      ResponseBuffer resp_buf = { .arena = arena };
      resp_buf.cap = 4096;
      resp_buf.buf = na_alloc(arena, resp_buf.cap);
      resp_buf.buf[0] = '\0';

      char read_buf[512];
      ssize_t n;
      while ((n = read(pipefd[0], read_buf, sizeof(read_buf))) > 0) {
        if (resp_buf.len + n >= resp_buf.cap) {
          resp_buf.cap = resp_buf.len + n + 4096;
          char *new_buf = na_alloc(arena, resp_buf.cap);
          if (!new_buf) break;
          memcpy(new_buf, resp_buf.buf, resp_buf.len);
          resp_buf.buf = new_buf;
        }
        memcpy(resp_buf.buf + resp_buf.len, read_buf, n);
        resp_buf.len += n;
        resp_buf.buf[resp_buf.len] = '\0';
      }
      close(pipefd[0]);

      ACNode *ac_root = get_global_ac_root();
      if (ac_root && resp_buf.len > 0) {
        char *redacted_buf = na_alloc(arena, resp_buf.len * 2 + 1);
        if (redacted_buf) {
          size_t new_len = redact_stream(ac_root, resp_buf.buf, redacted_buf, resp_buf.len);
          resp_buf.buf = redacted_buf;
          resp_buf.len = new_len;
        }
      }

      int status;
      waitpid(pid, &status, 0);
      unlink(temp_in_path);

      long exit_code = 0;
      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
      } else {
        exit_code = -1;
      }

      Jsonv_Value body_val = jsonv_val_undefined();
      if (resp_buf.len > 0) {
        Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
        if (temp_ctx) {
          if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)resp_buf.buf) ||
              jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)resp_buf.buf)) {
            jsonv_ctx_get_value(temp_ctx, &body_val);
          } else {
            char *str = allocate_jsonv_string(arena, resp_buf.buf, resp_buf.len);
            body_val = jsonv_val_str(str);
          }
        } else {
          char *str = allocate_jsonv_string(arena, resp_buf.buf, resp_buf.len);
          body_val = jsonv_val_str(str);
        }
      } else {
        body_val = jsonv_val_null();
      }

      Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
      char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
      char *k_body = allocate_jsonv_string(arena, "body", 4);
      jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(exit_code));
      jsonv_obj_set(jsonv_arena, outcome_obj, k_body, body_val);

      char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
      jsonv_obj_set(jsonv_arena, steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static JobNode *find_job_by_id(WorkflowAST *ast, StringView id) {
  /*#region*/
  // Find a job in the topologically sorted list matching id.
  JobNode *curr = ast->jobs_head;
  while (curr) {
    if (sv_compare(curr->id, id) == 0) return curr;
    curr = curr->next_sorted;
  }
  return NULL;
  /*#endregion*/
}

static void propagate_control_skips(WorkflowAST *ast, JobNode *job);

static void mark_job_skipped(WorkflowAST *ast, StringView id) {
  /*#region*/
  // Mark target job as skipped and recursively skip its branches.
  JobNode *j = find_job_by_id(ast, id);
  if (j && j->execution_state != STATE_SKIPPED) {
    j->execution_state = STATE_SKIPPED;
    propagate_control_skips(ast, j);
  }
  /*#endregion*/
}

static void propagate_control_skips(WorkflowAST *ast, JobNode *job) {
  /*#region*/
  // Propagate skipped state to all child branches of the skipped control node.
  if (job->type == NODE_IF) {
    for (size_t i = 0; i < job->spec.binary_if.then_count; i++) {
      mark_job_skipped(ast, job->spec.binary_if.then_branch[i]);
    }
    for (size_t i = 0; i < job->spec.binary_if.else_count; i++) {
      mark_job_skipped(ast, job->spec.binary_if.else_branch[i]);
    }
  } else if (job->type == NODE_SWITCH) {
    SwitchCase *sc = job->spec.multi_switch.cases;
    while (sc) {
      for (size_t i = 0; i < sc->then_count; i++) {
        mark_job_skipped(ast, sc->then_branch[i]);
      }
      sc = sc->next;
    }
    for (size_t i = 0; i < job->spec.multi_switch.default_count; i++) {
      mark_job_skipped(ast, job->spec.multi_switch.default_branch[i]);
    }
  } else if (job->type == NODE_FORK) {
    for (size_t i = 0; i < job->spec.fork_node.branch_count; i++) {
      mark_job_skipped(ast, job->spec.fork_node.branches[i]);
    }
  }
  /*#endregion*/
}


typedef struct ActiveJob ActiveJob;
struct ActiveJob {
  JobNode *job;
  StepNode *curr_step;
  Jsonv_Value steps_state_obj;
  Jsonv_Value local_vars;

  // Loop support
  bool is_loop;
  size_t loop_iter;
  size_t max_iterations;
  Jsonv_Value loop_history_obj;
  char loop_stream_file_path[256];
  size_t loop_stream_record_offset;

  // Active HTTP transport fields
  void *easy_handle;
  ResponseBuffer resp_buf;

  // Active Plugin fields
  PluginExecutor plugin_exec;

  // Retry tracking fields
  int curr_step_retry_attempt;
  struct timeval next_retry_time;
  bool is_waiting_retry;

  Arena *loop_arena;

  // Caching/validation fields
  bool is_validating;
  char cached_key[65];
  char cached_etag[128];
  char cached_last_modified[128];
  char *cached_output_payload;

  ActiveJob *next;
};

static void push_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars) {
  /*#region*/
  if (local_vars.tag == JSONV_VAL_OBJ && context_val.tag == JSONV_VAL_OBJ) {
    Jsonv_Obj *local_obj = local_vars.as.p;
    int len = jsonv_obj_length(local_obj);
    for (int i = 0; i < len; i++) {
      const char *key = jsonv_obj_key_at(local_obj, i);
      Jsonv_Value val = jsonv_obj_val_at(local_obj, i);
      jsonv_obj_set(jsonv_arena, context_val.as.p, key, val);
    }
  }
  /*#endregion*/
}

static void pop_local_vars(Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value local_vars) {
  /*#region*/
  if (local_vars.tag == JSONV_VAL_OBJ && context_val.tag == JSONV_VAL_OBJ) {
    Jsonv_Obj *local_obj = local_vars.as.p;
    int len = jsonv_obj_length(local_obj);
    for (int i = 0; i < len; i++) {
      const char *key = jsonv_obj_key_at(local_obj, i);
      jsonv_obj_set(jsonv_arena, context_val.as.p, key, jsonv_val_undefined());
    }
  }
  /*#endregion*/
}

static int32_t evaluate_job_variables(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, JobNode *job, Jsonv_Value local_vars) {
  /*#region*/
  if (!job->variables_head) return ERR_SUCCESS;
  
  VariableAST *var = job->variables_head;
  while (var) {
    Jsonv_Value val = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, var->expression, jsonv_arena, *context_val, &val);
    if (status == ERR_SUCCESS) {
      char *name_cstr = allocate_jsonv_string(arena, var->name.data, var->name.length);
      if (var->visibility == VAR_PRIVATE) {
        if (local_vars.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, local_vars.as.p, name_cstr, val);
        }
        jsonv_obj_set(jsonv_arena, context_val->as.p, name_cstr, val);
      } else {
        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Value jobs_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ) {
          Jsonv_Value job_item_obj;
          if (jsonv_obj_get(jobs_obj.as.p, job_id_cstr, &job_item_obj) && job_item_obj.tag == JSONV_VAL_OBJ) {
            Jsonv_Value outputs_obj;
            char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
            if (!jsonv_obj_get(job_item_obj.as.p, k_outputs, &outputs_obj) || outputs_obj.tag != JSONV_VAL_OBJ) {
              outputs_obj.tag = JSONV_VAL_OBJ;
              outputs_obj.as.p = jsonv_obj_new(jsonv_arena, NULL);
              jsonv_obj_set(jsonv_arena, job_item_obj.as.p, k_outputs, outputs_obj);
            }
            jsonv_obj_set(jsonv_arena, outputs_obj.as.p, name_cstr, val);
          }
        }
      }
    }
    var = var->next;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t evaluate_step_variables(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, JobNode *job, StepNode *step, Jsonv_Value local_vars) {
  /*#region*/
  if (!step->variables_head) return ERR_SUCCESS;
  
  VariableAST *var = step->variables_head;
  while (var) {
    Jsonv_Value val = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, var->expression, jsonv_arena, *context_val, &val);
    if (status == ERR_SUCCESS) {
      char *name_cstr = allocate_jsonv_string(arena, var->name.data, var->name.length);
      if (var->visibility == VAR_PRIVATE) {
        if (local_vars.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, local_vars.as.p, name_cstr, val);
        }
        jsonv_obj_set(jsonv_arena, context_val->as.p, name_cstr, val);
      } else {
        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Value jobs_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ) {
          Jsonv_Value job_item_obj;
          if (jsonv_obj_get(jobs_obj.as.p, job_id_cstr, &job_item_obj) && job_item_obj.tag == JSONV_VAL_OBJ) {
            Jsonv_Value outputs_obj;
            char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
            if (!jsonv_obj_get(job_item_obj.as.p, k_outputs, &outputs_obj) || outputs_obj.tag != JSONV_VAL_OBJ) {
              outputs_obj.tag = JSONV_VAL_OBJ;
              outputs_obj.as.p = jsonv_obj_new(jsonv_arena, NULL);
              jsonv_obj_set(jsonv_arena, job_item_obj.as.p, k_outputs, outputs_obj);
            }
            jsonv_obj_set(jsonv_arena, outputs_obj.as.p, name_cstr, val);
          }
        }
      }
    }
    var = var->next;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static bool is_edge_satisfied(JobNode *job, size_t d, JobNode *dep) {
  /*#region*/
  uint8_t mask = 0;
  if (job->depends_on_conditions) {
    mask = job->depends_on_conditions[d];
  } else {
    if (job->type == NODE_JOIN && sv_equals_cstr(job->spec.join_node.strategy, "all")) {
      mask = DEP_COND_SUCCESS | DEP_COND_SKIP;
    } else {
      mask = DEP_COND_SUCCESS;
    }
  }

  if (dep->execution_state == STATE_SUCCEEDED && (mask & DEP_COND_SUCCESS)) return true;
  if (dep->execution_state == STATE_FAILED && (mask & DEP_COND_FAILURE)) return true;
  if (dep->execution_state == STATE_SKIPPED && (mask & DEP_COND_SKIP)) return true;
  return false;
  /*#endregion*/
}

static bool is_ready(JobNode *job) {
  /*#region*/
  // Determine if all upstream dependencies of a non-join job are completed.
  if (job->execution_state != STATE_PENDING) return false;
  if (job->type == NODE_JOIN) return false; // Evaluated dynamically

  if (job->dependency_count > 0) {
    for (size_t d = 0; d < job->dependency_count; d++) {
      JobNode *dep = job->depends_on_nodes[d];
      if (dep->execution_state == STATE_PENDING || dep->execution_state == STATE_RUNNING) {
        return false;
      }
    }
  }
  return true;
  /*#endregion*/
}

static void update_job_states(WorkflowAST *ast) {
  /*#region*/
  // Evaluate joins and propagate skipped/failed states topologically.
  bool changed = true;
  while (changed) {
    changed = false;
    JobNode *job = ast->jobs_head;
    while (job) {
      if (job->execution_state == STATE_PENDING) {
        if (job->dependency_count > 0) {
          size_t num_succeeded = 0;
          size_t num_failed = 0;
          size_t num_skipped = 0;
          size_t num_completed = 0;
          size_t num_satisfied = 0;

          for (size_t d = 0; d < job->dependency_count; d++) {
            JobNode *dep = job->depends_on_nodes[d];
            if (dep->execution_state == STATE_SUCCEEDED) {
              num_succeeded++;
              num_completed++;
            } else if (dep->execution_state == STATE_FAILED) {
              num_failed++;
              num_completed++;
            } else if (dep->execution_state == STATE_SKIPPED) {
              num_skipped++;
              num_completed++;
            }

            if (dep->execution_state == STATE_SUCCEEDED ||
                dep->execution_state == STATE_FAILED ||
                dep->execution_state == STATE_SKIPPED) {
              if (is_edge_satisfied(job, d, dep)) {
                num_satisfied++;
              }
            }
          }

          if (job->type == NODE_JOIN) {
            bool join_ok = false;
            bool join_evaluated = false;

            if (job->depends_on_conditions != NULL) {
              // Edge-based conditions specified
              if (sv_equals_cstr(job->spec.join_node.strategy, "all")) {
                if (num_completed == job->dependency_count) {
                  join_ok = (num_satisfied == job->dependency_count);
                  join_evaluated = true;
                }
              } else if (sv_equals_cstr(job->spec.join_node.strategy, "any")) {
                if (num_satisfied > 0) {
                  join_ok = true;
                  join_evaluated = true;
                } else if (num_completed == job->dependency_count) {
                  join_ok = false;
                  join_evaluated = true;
                }
              } else if (sv_equals_cstr(job->spec.join_node.strategy, "n_required")) {
                if (num_satisfied >= job->spec.join_node.n_required) {
                  join_ok = true;
                  join_evaluated = true;
                } else if (num_completed == job->dependency_count) {
                  join_ok = false;
                  join_evaluated = true;
                }
              }
            } else {
              // Backward compatibility fallback
              if (sv_equals_cstr(job->spec.join_node.strategy, "all")) {
                if (num_completed == job->dependency_count) {
                  join_ok = (num_succeeded + num_skipped == job->dependency_count);
                  join_evaluated = true;
                }
              } else if (sv_equals_cstr(job->spec.join_node.strategy, "any")) {
                if (num_succeeded > 0) {
                  join_ok = true;
                  join_evaluated = true;
                } else if (num_completed == job->dependency_count) {
                  join_ok = false;
                  join_evaluated = true;
                }
              } else if (sv_equals_cstr(job->spec.join_node.strategy, "n_required")) {
                if (num_succeeded >= job->spec.join_node.n_required) {
                  join_ok = true;
                  join_evaluated = true;
                } else if (num_completed == job->dependency_count) {
                  join_ok = false;
                  join_evaluated = true;
                }
              }
            }

            if (join_evaluated) {
              if (join_ok) {
                job->execution_state = STATE_SUCCEEDED;
              } else {
                job->execution_state = STATE_FAILED;
              }
              changed = true;
            }
          } else {
            // Non-join nodes
            if (job->depends_on_conditions != NULL) {
              // Edge-based conditions specified
              if (num_completed == job->dependency_count) {
                if (num_satisfied < job->dependency_count) {
                  job->execution_state = STATE_SKIPPED;
                  propagate_control_skips(ast, job);
                  changed = true;
                }
              }
            } else {
              // Backward compatibility fallback
              if (num_completed == job->dependency_count) {
                if (num_failed > 0) {
                  job->execution_state = STATE_FAILED;
                  propagate_control_skips(ast, job);
                  changed = true;
                } else if (num_skipped == job->dependency_count) {
                  job->execution_state = STATE_SKIPPED;
                  propagate_control_skips(ast, job);
                  changed = true;
                }
              }
            }
          }
        }
      }
      job = job->next_sorted;
    }
  }
  /*#endregion*/
}

static void register_loop_job_outcome(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj) {
  /*#region*/
  char *job_id_cstr = allocate_jsonv_string(arena, aj->job->id.data, aj->job->id.length);
  Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
  char *k_steps = allocate_jsonv_string(arena, "steps", 5);
  Jsonv_Value steps_obj = (aj->loop_history_obj.tag == JSONV_VAL_OBJ) ? aj->loop_history_obj : aj->steps_state_obj;
  if (steps_obj.tag == JSONV_VAL_UNDEFINED) {
    steps_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
  }
  jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, steps_obj);

  Jsonv_Value jobs_val_obj;
  char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
  if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
    jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
  }
  /*#endregion*/
}

typedef struct {
  Arena *parent_arena;
  Arena *loop_arena;
  Jsonv_Arena *jsonv_arena;
  Jsonv_Arr *chunk_arr;
  size_t offset;
  size_t limit;
  size_t count;
  bool error;
} NestorChunkBuilderContext;

static void nestor_chunk_match_cb(void *user_data, Jsonata_ValType type, const char *val, size_t val_len) {
  /*#region*/
  (void)type;
  NestorChunkBuilderContext *ctx = (NestorChunkBuilderContext *)user_data;
  if (ctx->error) return;

  if (ctx->count >= ctx->offset && ctx->count < ctx->offset + ctx->limit) {
    char *tmp = na_alloc(ctx->loop_arena, val_len + 1);
    if (!tmp) {
      ctx->error = true;
      return;
    }
    memcpy(tmp, val, val_len);
    tmp[val_len] = '\0';

    Jsonv_Obj *elem = jsonv_obj_new(ctx->jsonv_arena, NULL);
    if (!elem) {
      ctx->error = true;
      return;
    }
    char *k_id = allocate_jsonv_string(ctx->parent_arena, "id", 2);
    int64_t id_val = atoll(tmp);
    jsonv_obj_set(ctx->jsonv_arena, elem, k_id, jsonv_val_int(id_val));

    int len = jsonv_arr_length(ctx->chunk_arr);
    jsonv_arr_set(ctx->jsonv_arena, ctx->chunk_arr, len, jsonv_val_obj(elem));
  }
  ctx->count++;
  /*#endregion*/
}

static int32_t start_loop_iteration(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj) {
  /*#region*/
  // Setup next loop iteration index and variables in workflow context.
  if (aj->loop_arena) {
    arena_destroy(aj->loop_arena);
  }
  aj->loop_arena = arena_create(256 * 1024);
  if (!aj->loop_arena) {
    return ERR_OOM;
  }

  JobNode *job = aj->job;
  if (sv_equals_cstr(job->spec.loop_node.loop_type, "while")) {
    if (aj->loop_iter >= aj->max_iterations) {
      job->execution_state = STATE_FAILED;
      return ERR_LOOP_MAX_ITERATIONS;
    }

    char *k_index = allocate_jsonv_string(arena, "index", 5);
    jsonv_obj_set(jsonv_arena, context_val->as.p, k_index, jsonv_val_int((int64_t)aj->loop_iter));

    Jsonv_Value eval_res = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, job->spec.loop_node.condition, jsonv_arena, *context_val, &eval_res);
    if (status != ERR_SUCCESS || !is_truthy(eval_res)) {
      job->execution_state = STATE_SUCCEEDED;
      aj->curr_step = NULL;
      register_loop_job_outcome(arena, jsonv_arena, context_val, aj);
      return ERR_SUCCESS;
    }

    if (aj->loop_iter == 0) {
      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      aj->loop_history_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

      char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
      Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
      jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->loop_history_obj);
      Jsonv_Value jobs_val_obj;
      char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
      if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
        jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
      }
    } else {
      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
    }
    aj->curr_step = job->spec.loop_node.steps_head;

  } else if (sv_equals_cstr(job->spec.loop_node.loop_type, "for_each")) {
    Jsonv_Value items_val = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, job->spec.loop_node.items, jsonv_arena, *context_val, &items_val);
    if (status != ERR_SUCCESS) {
      job->execution_state = STATE_FAILED;
      return status;
    }

    if (items_val.tag == JSONV_VAL_ARRAY) {
      Jsonv_Arr *arr = items_val.as.p;
      int len = jsonv_arr_length(arr);
      if ((int)aj->loop_iter >= len) {
        job->execution_state = STATE_SUCCEEDED;
        aj->curr_step = NULL;
        register_loop_job_outcome(arena, jsonv_arena, context_val, aj);
        return ERR_SUCCESS;
      }

      if (aj->loop_iter >= aj->max_iterations) {
        job->execution_state = STATE_FAILED;
        return ERR_LOOP_MAX_ITERATIONS;
      }

      Jsonv_Value item_val;
      if (!jsonv_arr_get(arr, (int)aj->loop_iter, &item_val)) {
        item_val = jsonv_val_null();
      }

      char *k_index = allocate_jsonv_string(arena, "index", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_index, jsonv_val_int((int64_t)aj->loop_iter));

      char *k_item = allocate_jsonv_string(arena, "item", 4);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_item, item_val);

      if (aj->loop_iter == 0) {
        aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        aj->loop_history_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->loop_history_obj);
        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
        }
      } else {
        aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
      }
      aj->curr_step = job->spec.loop_node.steps_head;
    } else {
      if (aj->loop_iter > 0) {
        job->execution_state = STATE_SUCCEEDED;
        aj->curr_step = NULL;
        register_loop_job_outcome(arena, jsonv_arena, context_val, aj);
        return ERR_SUCCESS;
      }

      char *k_index = allocate_jsonv_string(arena, "index", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_index, jsonv_val_int(0));

      char *k_item = allocate_jsonv_string(arena, "item", 4);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_item, items_val);

      if (aj->loop_iter == 0) {
        aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        aj->loop_history_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->loop_history_obj);
        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
        }
      } else {
        aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
      }
      aj->curr_step = job->spec.loop_node.steps_head;
    }
  } else if (sv_equals_cstr(job->spec.loop_node.loop_type, "stream_chunk")) {
    /*#region*/
    if (aj->loop_iter == 0) {
      Jsonv_Value source_val = jsonv_val_undefined();
      int32_t status = evaluate_expression(arena, job->spec.loop_node.source, jsonv_arena, *context_val, &source_val);
      if (status != ERR_SUCCESS || source_val.tag != JSONV_VAL_STRING) {
        job->execution_state = STATE_FAILED;
        return status != ERR_SUCCESS ? status : ERR_MISSING_VAR;
      }
      strncpy(aj->loop_stream_file_path, (const char *)source_val.as.p, sizeof(aj->loop_stream_file_path) - 1);
      aj->loop_stream_file_path[sizeof(aj->loop_stream_file_path) - 1] = '\0';
      aj->loop_stream_record_offset = 0;
    }

    int fd = open(aj->loop_stream_file_path, O_RDONLY);
    if (fd < 0) {
      job->execution_state = STATE_FAILED;
      return ERR_HTTP_TRANSPORT;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
      close(fd);
      job->execution_state = STATE_SUCCEEDED;
      aj->curr_step = NULL;
      register_loop_job_outcome(arena, jsonv_arena, context_val, aj);
      return ERR_SUCCESS;
    }

    void *addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
      close(fd);
      job->execution_state = STATE_FAILED;
      return ERR_HTTP_TRANSPORT;
    }

    char filter_key[256] = "data";
    if (job->spec.loop_node.items.length > 0 && job->spec.loop_node.items.length < sizeof(filter_key)) {
      memcpy(filter_key, job->spec.loop_node.items.data, job->spec.loop_node.items.length);
      filter_key[job->spec.loop_node.items.length] = '\0';
    }

    Jsonv_Arr *chunk_arr = jsonv_arr_new(jsonv_arena);
    NestorChunkBuilderContext builder_ctx = {
      .parent_arena = arena,
      .loop_arena = aj->loop_arena,
      .jsonv_arena = jsonv_arena,
      .chunk_arr = chunk_arr,
      .offset = aj->loop_stream_record_offset,
      .limit = job->spec.loop_node.chunk_record_limit,
      .count = 0,
      .error = false
    };

    Jsonata_Arena *jsonata_arena = nestor_jsonata_arena_new(aj->loop_arena);
    Jsonata_StreamFilter *filter = jsonata_stream_filter_create(filter_key, nestor_chunk_match_cb, &builder_ctx, jsonata_arena);
    if (!filter) {
      munmap(addr, st.st_size);
      close(fd);
      job->execution_state = STATE_FAILED;
      return ERR_OOM;
    }

    jsonv_sax_callbacks callbacks = {
      .on_begin_object = jsonata_stream_filter_on_begin_object,
      .on_end_object = jsonata_stream_filter_on_end_object,
      .on_begin_array = jsonata_stream_filter_on_begin_array,
      .on_end_array = jsonata_stream_filter_on_end_array,
      .on_object_key = jsonata_stream_filter_on_object_key,
      .on_string = jsonata_stream_filter_on_string,
      .on_number = jsonata_stream_filter_on_number,
      .on_boolean = jsonata_stream_filter_on_boolean,
      .on_null = jsonata_stream_filter_on_null
    };

    jsonv_parse_sax((const unsigned char *)addr, st.st_size, &callbacks, filter);

    munmap(addr, st.st_size);
    close(fd);

    if (builder_ctx.error) {
      job->execution_state = STATE_FAILED;
      return ERR_HTTP_TRANSPORT;
    }

    int chunk_len = jsonv_arr_length(chunk_arr);
    if (chunk_len == 0) {
      job->execution_state = STATE_SUCCEEDED;
      aj->curr_step = NULL;
      register_loop_job_outcome(arena, jsonv_arena, context_val, aj);
      return ERR_SUCCESS;
    }

    if (aj->loop_iter >= aj->max_iterations) {
      job->execution_state = STATE_FAILED;
      return ERR_LOOP_MAX_ITERATIONS;
    }

    char *k_index = allocate_jsonv_string(arena, "index", 5);
    jsonv_obj_set(jsonv_arena, context_val->as.p, k_index, jsonv_val_int((int64_t)aj->loop_iter));

    char *k_chunk = allocate_jsonv_string(arena, "chunk", 5);
    jsonv_obj_set(jsonv_arena, context_val->as.p, k_chunk, jsonv_val_arr(chunk_arr));

    if (aj->loop_iter == 0) {
      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      aj->loop_history_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

      char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
      Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
      jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->loop_history_obj);
      Jsonv_Value jobs_val_obj;
      char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
      if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
        jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
      }
    } else {
      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
    }

    aj->loop_stream_record_offset += chunk_len;
    aj->curr_step = job->spec.loop_node.steps_head;
    /*#endregion*/
  }
  return ERR_SUCCESS;
  /*#endregion*/
}


static void save_step_outcome(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, StepNode *step, Jsonv_Obj *outcome_obj) {
  /*#region*/
  if (step->outputs_head) {
    Arena *temp_arena = arena_create(256 * 1024);
    if (temp_arena) {
      Jsonv_Arena *temp_jsonv_arena = jsonv_arena_new_custom(&my_jsonv_ops, temp_arena);
      if (temp_jsonv_arena) {
        Jsonv_Obj *projected_outputs = jsonv_obj_new(temp_jsonv_arena, NULL);
        Jsonv_Value context_val = jsonv_val_obj(outcome_obj);

        VariableAST *curr = step->outputs_head;
        while (curr) {
          Jsonv_Value val = jsonv_val_undefined();
          int32_t status = evaluate_expression(temp_arena, curr->expression, temp_jsonv_arena, context_val, &val);
          char *name_cstr = allocate_jsonv_string(temp_arena, curr->name.data, curr->name.length);
          if (status == ERR_SUCCESS) {
            jsonv_obj_set(temp_jsonv_arena, projected_outputs, name_cstr, val);
          } else {
            jsonv_obj_set(temp_jsonv_arena, projected_outputs, name_cstr, jsonv_val_null());
          }
          curr = curr->next;
        }

        int sz = jsonv_serialize(jsonv_val_obj(projected_outputs), NULL, 0);
        if (sz >= 0) {
          char *serialized_buf = na_alloc(arena, sz + 1);
          if (serialized_buf) {
            jsonv_serialize(jsonv_val_obj(projected_outputs), serialized_buf, sz + 1);

            arena_destroy(temp_arena);
            temp_arena = NULL;

            Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
            if (temp_ctx) {
              if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)serialized_buf)) {
                Jsonv_Value main_projected_val;
                jsonv_ctx_get_value(temp_ctx, &main_projected_val);

                char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
                jsonv_obj_set(jsonv_arena, outcome_obj, k_outputs, main_projected_val);

                char *k_body = allocate_jsonv_string(arena, "body", 4);
                char *k_stderr = allocate_jsonv_string(arena, "stderr", 6);
                jsonv_obj_set(jsonv_arena, outcome_obj, k_body, jsonv_val_null());
                jsonv_obj_set(jsonv_arena, outcome_obj, k_stderr, jsonv_val_null());
              }
            }
          }
        }
      }
      if (temp_arena) {
        arena_destroy(temp_arena);
      }
    }
  }

  char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
  jsonv_obj_set(jsonv_arena, aj->steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));

  if (aj->is_loop && aj->loop_history_obj.tag == JSONV_VAL_OBJ) {
    Jsonv_Value existing_val;
    if (jsonv_obj_get(aj->loop_history_obj.as.p, step_id_cstr, &existing_val) && existing_val.tag == JSONV_VAL_ARRAY) {
      jsonv_arr_set(jsonv_arena, existing_val.as.p, (int)aj->loop_iter, jsonv_val_obj(outcome_obj));
    } else {
      Jsonv_Arr *arr = jsonv_arr_new(jsonv_arena);
      jsonv_arr_set(jsonv_arena, arr, (int)aj->loop_iter, jsonv_val_obj(outcome_obj));
      jsonv_obj_set(jsonv_arena, aj->loop_history_obj.as.p, step_id_cstr, jsonv_val_arr(arr));
    }
  }
  /*#endregion*/
}

static int32_t complete_http_step_async(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, long status_code) {
  /*#region*/
  // Complete processing and bind HTTP response variables back into workspace.
  if (aj->is_validating && status_code == 304) {
    Jsonv_Value cached_steps_val = jsonv_val_undefined();
    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
    if (temp_ctx && jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)aj->cached_output_payload)) {
      jsonv_ctx_get_value(temp_ctx, &cached_steps_val);
      if (cached_steps_val.tag == JSONV_VAL_OBJ) {
        aj->steps_state_obj = cached_steps_val;
      }
    }
    return ERR_SUCCESS;
  }

  StepNode *step = aj->curr_step;

  ACNode *ac_root = get_global_ac_root();
  if (ac_root && aj->resp_buf.len > 0) {
    char *redacted_buf = na_alloc(arena, aj->resp_buf.len * 2 + 1);
    if (redacted_buf) {
      size_t new_len = redact_stream(ac_root, aj->resp_buf.buf, redacted_buf, aj->resp_buf.len);
      aj->resp_buf.buf = redacted_buf;
      aj->resp_buf.len = new_len;
    }
  }
  Jsonv_Value body_val = jsonv_val_undefined();
  Jsonv_Value stream_val = jsonv_val_undefined();
  if (aj->resp_buf.is_stream) {
    // If streaming is enabled, we create a placeholder body object containing
    // the target file path. This path will trigger lazy property lookups dynamically.
    Jsonv_Obj *stream_body_obj = jsonv_obj_new(jsonv_arena, NULL);
    char *k_sfp = allocate_jsonv_string(arena, "_stream_file_path", 17);
    char *sfp_val = allocate_jsonv_string(arena, aj->resp_buf.stream_file_path, strlen(aj->resp_buf.stream_file_path));
    jsonv_obj_set(jsonv_arena, stream_body_obj, k_sfp, jsonv_val_str(sfp_val));
    body_val = jsonv_val_obj(stream_body_obj);
    stream_val = jsonv_val_str(sfp_val);
  } else if (aj->resp_buf.len > 0) {
    Jsonv_Config config = {0};
    config.default_block_size = 4096;
    config.max_limit = 16 * 1024 * 1024;
    config.max_depth = 128;
    config.max_values = 100000;
    config.max_objects = 50000;
    config.max_array = 50000;
    config.max_string_bytes = 4 * 1024 * 1024;

    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, &config, NULL);
    if (temp_ctx) {
      if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)aj->resp_buf.buf) ||
          jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)aj->resp_buf.buf)) {
        jsonv_ctx_get_value(temp_ctx, &body_val);
      } else {
        char *str = allocate_jsonv_string(arena, aj->resp_buf.buf, aj->resp_buf.len);
        body_val = jsonv_val_str(str);
      }
    } else {
      char *str = allocate_jsonv_string(arena, aj->resp_buf.buf, aj->resp_buf.len);
      body_val = jsonv_val_str(str);
    }
  } else {
    body_val = jsonv_val_null();
  }

  Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
  char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
  char *k_body = allocate_jsonv_string(arena, "body", 4);
  jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(status_code));
  jsonv_obj_set(jsonv_arena, outcome_obj, k_body, body_val);
  if (aj->resp_buf.is_stream) {
    char *k_stream = allocate_jsonv_string(arena, "stream", 6);
    jsonv_obj_set(jsonv_arena, outcome_obj, k_stream, stream_val);
  }

  save_step_outcome(arena, jsonv_arena, aj, step, outcome_obj);

  return ERR_SUCCESS;
  /*#endregion*/
}


static bool handle_step_failure(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj, const char *err_msg) {
  /*#region*/
  (void)arena;
  (void)jsonv_arena;
  (void)context_val;
  StepNode *step = aj->curr_step;
  if (aj->curr_step_retry_attempt < step->retry_attempts) {
    aj->curr_step_retry_attempt++;

    long initial_delay_ms = 1000;
    if (step->retry_delay.length > 0) {
      initial_delay_ms = parse_duration_ms(step->retry_delay);
    }

    long delay_ms = initial_delay_ms;
    if (sv_equals_cstr(step->retry_backoff, "exponential")) {
      long factor = 1;
      for (int i = 1; i < aj->curr_step_retry_attempt; i++) {
        factor *= 2;
      }
      delay_ms = initial_delay_ms * factor;
    } else if (sv_equals_cstr(step->retry_backoff, "linear")) {
      delay_ms = initial_delay_ms * aj->curr_step_retry_attempt;
    }

    fprintf(stderr, "STEP FAILURE: %s. Retrying step %.*s (attempt %d/%d) in %ld ms...\n",
            err_msg, (int)step->id.length, step->id.data,
            aj->curr_step_retry_attempt, step->retry_attempts, delay_ms);

    struct timeval now;
    gettimeofday(&now, NULL);
    long target_usec = now.tv_usec + (delay_ms % 1000) * 1000;
    aj->next_retry_time.tv_sec = now.tv_sec + (delay_ms / 1000) + (target_usec / 1000000);
    aj->next_retry_time.tv_usec = target_usec % 1000000;
    aj->is_waiting_retry = true;

    aj->easy_handle = NULL;
    aj->plugin_exec.child_pid = 0;

    return true;
  }
  return false;
  /*#endregion*/
}

static int32_t apply_step_fallback(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, ActiveJob *aj, WorkflowAST *ast, Transport *transport) {
  /*#region*/
  StepNode *step = aj->curr_step;
  fprintf(stderr, "STEP FAILURE. Applying fallback for step %.*s...\n",
          (int)step->id.length, step->id.data);

  Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
  char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
  char *k_body = allocate_jsonv_string(arena, "body", 4);
  char *k_stderr = allocate_jsonv_string(arena, "stderr", 6);
  jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(200));
  jsonv_obj_set(jsonv_arena, outcome_obj, k_body, step->fallback);
  jsonv_obj_set(jsonv_arena, outcome_obj, k_stderr, jsonv_val_str(allocate_jsonv_string(arena, "", 0)));

  save_step_outcome(arena, jsonv_arena, aj, step, outcome_obj);

  aj->curr_step_retry_attempt = 0;
  aj->curr_step = aj->curr_step->next;

  evaluate_job_variables(arena, jsonv_arena, context_val, aj->job, aj->local_vars);
  return advance_active_job(arena, jsonv_arena, ast, context_val, aj, transport);
  /*#endregion*/
}

static void process_ipc_request(Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value *context_val, int client_fd, const char *req_str) {
  /*#region*/
  Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
  if (!temp_ctx) return;
  if (!jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)req_str)) {
    const char *err_resp = "{\"status\":\"error\",\"message\":\"invalid json request\"}\n";
    send(client_fd, err_resp, strlen(err_resp), 0);
    return;
  }
  Jsonv_Value req_val;
  jsonv_ctx_get_value(temp_ctx, &req_val);
  if (req_val.tag != JSONV_VAL_OBJ) {
    const char *err_resp = "{\"status\":\"error\",\"message\":\"request must be an object\"}\n";
    send(client_fd, err_resp, strlen(err_resp), 0);
    return;
  }

  Jsonv_Value v_action;
  if (!jsonv_obj_get(req_val.as.p, "action", &v_action) || v_action.tag != JSONV_VAL_STRING) {
    const char *err_resp = "{\"status\":\"error\",\"message\":\"missing action\"}\n";
    send(client_fd, err_resp, strlen(err_resp), 0);
    return;
  }
  const char *action_str = (const char *)v_action.as.p;

  if (strcmp(action_str, "get") == 0) {
    Jsonv_Value v_path;
    if (!jsonv_obj_get(req_val.as.p, "path", &v_path) || v_path.tag != JSONV_VAL_STRING) {
      const char *err_resp = "{\"status\":\"error\",\"message\":\"missing path\"}\n";
      send(client_fd, err_resp, strlen(err_resp), 0);
      return;
    }
    StringView expr_sv = { (const char *)v_path.as.p, jsonv_val_str_len(v_path) };
    Jsonv_Value eval_res = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, expr_sv, jsonv_arena, *context_val, &eval_res);
    if (status == ERR_SUCCESS) {
      char *serialized = "";
      serialize_jsonv_value(arena, eval_res, &serialized);
      
      size_t resp_cap = strlen(serialized) + 64;
      char *resp_buf = na_alloc(arena, resp_cap);
      if (resp_buf) {
        snprintf(resp_buf, resp_cap, "{\"status\":\"success\",\"value\":%s}\n", serialized);
        send(client_fd, resp_buf, strlen(resp_buf), 0);
      }
    } else {
      const char *err_resp = "{\"status\":\"error\",\"message\":\"path not found or evaluation failed\"}\n";
      send(client_fd, err_resp, strlen(err_resp), 0);
    }
  } else if (strcmp(action_str, "set") == 0) {
    Jsonv_Value v_path;
    if (!jsonv_obj_get(req_val.as.p, "path", &v_path) || v_path.tag != JSONV_VAL_STRING) {
      const char *err_resp = "{\"status\":\"error\",\"message\":\"missing path\"}\n";
      send(client_fd, err_resp, strlen(err_resp), 0);
      return;
    }
    Jsonv_Value v_value;
    if (!jsonv_obj_get(req_val.as.p, "value", &v_value)) {
      const char *err_resp = "{\"status\":\"error\",\"message\":\"missing value\"}\n";
      send(client_fd, err_resp, strlen(err_resp), 0);
      return;
    }
    
    StringView path_sv = { (const char *)v_path.as.p, jsonv_val_str_len(v_path) };
    if (path_sv.length > 10 && strncmp(path_sv.data, "variables.", 10) == 0) {
      char var_key[128];
      size_t k_len = path_sv.length - 10;
      if (k_len < sizeof(var_key)) {
        memcpy(var_key, path_sv.data + 10, k_len);
        var_key[k_len] = '\0';
        
        Jsonv_Value vars_val;
        if (!jsonv_obj_get(context_val->as.p, "variables", &vars_val) || vars_val.tag != JSONV_VAL_OBJ) {
          vars_val = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
          char *k_vars = allocate_jsonv_string(arena, "variables", 9);
          jsonv_obj_set(jsonv_arena, context_val->as.p, k_vars, vars_val);
        }
        
        char *var_key_alloc = allocate_jsonv_string(arena, var_key, k_len);
        jsonv_obj_set(jsonv_arena, vars_val.as.p, var_key_alloc, v_value);
        
        const char *ok_resp = "{\"status\":\"success\"}\n";
        send(client_fd, ok_resp, strlen(ok_resp), 0);
      } else {
        const char *err_resp = "{\"status\":\"error\",\"message\":\"variable key too long\"}\n";
        send(client_fd, err_resp, strlen(err_resp), 0);
      }
    } else {
      const char *err_resp = "{\"status\":\"error\",\"message\":\"only setting paths under variables.* is supported\"}\n";
      send(client_fd, err_resp, strlen(err_resp), 0);
    }
  } else {
    const char *err_resp = "{\"status\":\"error\",\"message\":\"unknown action\"}\n";
    send(client_fd, err_resp, strlen(err_resp), 0);
  }
  /*#endregion*/
}



static int32_t complete_plugin_step_async(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, long exit_code) {
  /*#region*/
  StepNode *step = aj->curr_step;

  ACNode *ac_root = get_global_ac_root();
  if (ac_root && aj->plugin_exec.child_resp_buf.len > 0) {
    char *redacted_buf = na_alloc(arena, aj->plugin_exec.child_resp_buf.len * 2 + 1);
    if (redacted_buf) {
      size_t new_len = redact_stream(ac_root, aj->plugin_exec.child_resp_buf.buf, redacted_buf, aj->plugin_exec.child_resp_buf.len);
      aj->plugin_exec.child_resp_buf.buf = redacted_buf;
      aj->plugin_exec.child_resp_buf.len = new_len;
    }
  }

  if (ac_root && aj->plugin_exec.child_stderr_buf.len > 0) {
    char *redacted_stderr = na_alloc(arena, aj->plugin_exec.child_stderr_buf.len * 2 + 1);
    if (redacted_stderr) {
      size_t new_stderr_len = redact_stream(ac_root, aj->plugin_exec.child_stderr_buf.buf, redacted_stderr, aj->plugin_exec.child_stderr_buf.len);
      aj->plugin_exec.child_stderr_buf.buf = redacted_stderr;
      aj->plugin_exec.child_stderr_buf.len = new_stderr_len;
    }
  }

  Jsonv_Value body_val = jsonv_val_undefined();
  Jsonv_Value outputs_val = jsonv_val_undefined();
  bool has_outputs = false;

  if (aj->plugin_exec.child_resp_buf.len > 0) {
    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
    if (temp_ctx) {
      if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)aj->plugin_exec.child_resp_buf.buf) ||
          jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)aj->plugin_exec.child_resp_buf.buf)) {
        Jsonv_Value parsed_val;
        jsonv_ctx_get_value(temp_ctx, &parsed_val);
        if (parsed_val.tag == JSONV_VAL_OBJ) {
          Jsonv_Value outs;
          char *k_outs = allocate_jsonv_string(arena, "outputs", 7);
          if (jsonv_obj_get(parsed_val.as.p, k_outs, &outs) && outs.tag == JSONV_VAL_OBJ) {
            outputs_val = outs;
            has_outputs = true;
            Jsonv_Value sc;
            char *k_sc = allocate_jsonv_string(arena, "status_code", 11);
            if (jsonv_obj_get(parsed_val.as.p, k_sc, &sc) && sc.tag == JSONV_VAL_INT) {
              exit_code = sc.as.i;
            }
            body_val = jsonv_val_null();
          } else {
            body_val = parsed_val;
          }
        } else {
          body_val = parsed_val;
        }
      } else {
        char *str = allocate_jsonv_string(arena, aj->plugin_exec.child_resp_buf.buf, aj->plugin_exec.child_resp_buf.len);
        body_val = jsonv_val_str(str);
      }
    } else {
      char *str = allocate_jsonv_string(arena, aj->plugin_exec.child_resp_buf.buf, aj->plugin_exec.child_resp_buf.len);
      body_val = jsonv_val_str(str);
    }
  } else {
    body_val = jsonv_val_null();
  }

  Jsonv_Value stderr_val = jsonv_val_undefined();
  if (aj->plugin_exec.child_stderr_buf.len > 0) {
    char *str = allocate_jsonv_string(arena, aj->plugin_exec.child_stderr_buf.buf, aj->plugin_exec.child_stderr_buf.len);
    stderr_val = jsonv_val_str(str);
  } else {
    char *str = allocate_jsonv_string(arena, "", 0);
    stderr_val = jsonv_val_str(str);
  }

  Jsonv_Obj *outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
  char *k_status_code = allocate_jsonv_string(arena, "status_code", 11);
  char *k_body = allocate_jsonv_string(arena, "body", 4);
  char *k_stderr = allocate_jsonv_string(arena, "stderr", 6);
  jsonv_obj_set(jsonv_arena, outcome_obj, k_status_code, jsonv_val_int(exit_code));
  jsonv_obj_set(jsonv_arena, outcome_obj, k_body, body_val);
  jsonv_obj_set(jsonv_arena, outcome_obj, k_stderr, stderr_val);

  if (has_outputs) {
    char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
    jsonv_obj_set(jsonv_arena, outcome_obj, k_outputs, outputs_val);
  }

  save_step_outcome(arena, jsonv_arena, aj, step, outcome_obj);

  plugin_cleanup(&aj->plugin_exec);
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t build_outputs_ast(Arena *arena, Jsonv_Value v_outputs, VariableAST **out_head) {
  /*#region*/
  if (v_outputs.tag != JSONV_VAL_OBJ) return ERR_SUCCESS;
  int len = jsonv_obj_length(v_outputs.as.p);
  VariableAST *tail = NULL;
  for (int i = 0; i < len; i++) {
    const char *key = jsonv_obj_key_at(v_outputs.as.p, i);
    Jsonv_Value expr_val = jsonv_obj_val_at(v_outputs.as.p, i);
    if (expr_val.tag != JSONV_VAL_STRING) continue;

    VariableAST *var = na_alloc(arena, sizeof(VariableAST));
    if (!var) return ERR_OOM;
    memset(var, 0, sizeof(VariableAST));

    var->name.data = key;
    var->name.length = strlen(key);
    var->expression.data = expr_val.as.p;
    var->expression.length = jsonv_val_str_len(expr_val);

    if (!*out_head) {
      *out_head = var;
    } else {
      tail->next = var;
    }
    tail = var;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t advance_active_job(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, ActiveJob *aj, Transport *transport) {
  /*#region*/
  // Advance steps within an active job; executes non-blocking tasks inline.
  if (aj->job->execution_state == STATE_FAILED || aj->is_waiting_retry) {
    return ERR_SUCCESS;
  }

  if (aj->job->type == NODE_WAIT_TIMER) {
    aj->job->execution_state = STATE_SUCCEEDED;
    char *job_id_cstr = allocate_jsonv_string(arena, aj->job->id.data, aj->job->id.length);
    Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
    Jsonv_Value jobs_val_obj;
    char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
    if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
      jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
    }
    return ERR_SUCCESS;
  }

  Arena *effective_arena = aj->loop_arena ? aj->loop_arena : arena;

  while (aj->curr_step != NULL && aj->easy_handle == NULL && aj->plugin_exec.child_pid == 0) {
    StepNode *step = aj->curr_step;
    evaluate_step_variables(arena, jsonv_arena, context_val, aj->job, step, aj->local_vars);
    if (step->is_http) {
      int32_t status = transport->ops->start_request(transport, effective_arena, jsonv_arena, *context_val, step, &aj->resp_buf, &aj->easy_handle);
      if (status != ERR_SUCCESS) {
        aj->job->execution_state = STATE_FAILED;
        return status;
      }
      break;
    } else if (step->is_provider) {
      char *prov_str = sv_to_cstring(arena, step->prov.provider);
      char *dot = strchr(prov_str, '.');
      char *operation = "";
      if (dot) {
        *dot = '\0';
        operation = dot + 1;
      }
      char *provider_name = prov_str;

      ProviderDef *pdef = NULL;
      if (ast->providers_map) {
        WorkspaceMap *map = (WorkspaceMap *)ast->providers_map;
        pdef = workspace_find_provider(map, (StringView){provider_name, strlen(provider_name)});
      }


      if (pdef && pdef->operations.tag == JSONV_VAL_OBJ) {
        Jsonv_Value op_val;
        if (jsonv_obj_get(pdef->operations.as.p, operation, &op_val) && op_val.tag == JSONV_VAL_OBJ) {
          ProviderConfigAST *pc = ast->providers_head;
          while (pc) {
            char *pc_name = sv_to_cstring(arena, pc->name);
            if (strcmp(pc_name, provider_name) == 0) {
              break;
            }
            pc = pc->next;
          }
          Jsonv_Value resolved_config = jsonv_val_undefined();
          if (pc) {
            resolved_config = resolve_json_value(arena, pc->config_val, jsonv_arena, *context_val);
          } else {
            resolved_config = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
          }

          Jsonv_Value resolved_args = resolve_json_value(arena, step->prov.args, jsonv_arena, *context_val);

          Jsonv_Obj *sub_root = jsonv_obj_new(jsonv_arena, NULL);
          jsonv_obj_set(jsonv_arena, sub_root, "inputs", resolved_args);

          Jsonv_Value sec_val;
          if (jsonv_obj_get(context_val->as.p, "secrets", &sec_val)) {
            jsonv_obj_set(jsonv_arena, sub_root, "secrets", sec_val);
          }
          Jsonv_Value env_val;
          if (jsonv_obj_get(context_val->as.p, "env", &env_val)) {
            jsonv_obj_set(jsonv_arena, sub_root, "env", env_val);
          }
          Jsonv_Value sub_ctx = jsonv_val_obj(sub_root);

          Jsonv_Value v_uses;
          if (jsonv_obj_get(op_val.as.p, "uses", &v_uses) && v_uses.tag == JSONV_VAL_STRING) {
            Jsonv_Value op_args_val = jsonv_val_undefined();
            jsonv_obj_get(op_val.as.p, "args", &op_args_val);

            Jsonv_Value resolved_op_args = resolve_json_value(arena, op_args_val, jsonv_arena, sub_ctx);

            StepNode temp_step;
            memset(&temp_step, 0, sizeof(StepNode));
            temp_step.id = step->id;

            Jsonv_Value v_outputs;
            if (jsonv_obj_get(op_val.as.p, "outputs", &v_outputs) && v_outputs.tag == JSONV_VAL_OBJ) {
              build_outputs_ast(arena, v_outputs, &temp_step.outputs_head);
            }

            char *uses_str = sv_to_cstring(arena, (StringView){v_uses.as.p, jsonv_val_str_len(v_uses)});
            if (strcmp(uses_str, "http") == 0) {
              temp_step.is_http = true;

              Jsonv_Value v_method = jsonv_val_undefined();
              Jsonv_Value v_url = jsonv_val_undefined();
              Jsonv_Value v_headers = jsonv_val_undefined();
              Jsonv_Value v_body = jsonv_val_undefined();
              if (resolved_op_args.tag == JSONV_VAL_OBJ) {
                jsonv_obj_get(resolved_op_args.as.p, "method", &v_method);
                jsonv_obj_get(resolved_op_args.as.p, "url", &v_url);
                jsonv_obj_get(resolved_op_args.as.p, "headers", &v_headers);
                jsonv_obj_get(resolved_op_args.as.p, "body", &v_body);
              }

              if (v_method.tag == JSONV_VAL_STRING) {
                temp_step.http.method.data = v_method.as.p;
                temp_step.http.method.length = jsonv_val_str_len(v_method);
              } else {
                temp_step.http.method = (StringView){"GET", 3};
              }

              if (v_url.tag == JSONV_VAL_STRING) {
                temp_step.http.url.data = v_url.as.p;
                temp_step.http.url.length = jsonv_val_str_len(v_url);
              }
              temp_step.http.headers = v_headers;
              temp_step.http.body = v_body;

              int32_t status = transport->ops->start_request(transport, effective_arena, jsonv_arena, *context_val, &temp_step, &aj->resp_buf, &aj->easy_handle);
              if (status != ERR_SUCCESS) {
                aj->job->execution_state = STATE_FAILED;
                return status;
              }
              break;
            } else {
              char *dot_pl = strchr(uses_str, '.');
              char *pl_operation = "";
              if (dot_pl && strcmp(dot_pl, ".so") != 0 && strcmp(dot_pl, ".dylib") != 0 && strcmp(dot_pl, ".dll") != 0 && strcmp(dot_pl, ".js") != 0) {
                *dot_pl = '\0';
                pl_operation = dot_pl + 1;
              }
              char *plugin_name = uses_str;

              Jsonv_Obj *payload_obj = jsonv_obj_new(jsonv_arena, NULL);
              char *k_op = allocate_jsonv_string(arena, "operation", 9);
              char *k_config = allocate_jsonv_string(arena, "configuration", 13);
              char *k_args = allocate_jsonv_string(arena, "args", 4);

              jsonv_obj_set(jsonv_arena, payload_obj, k_op, jsonv_val_str(allocate_jsonv_string(arena, pl_operation, strlen(pl_operation))));
              jsonv_obj_set(jsonv_arena, payload_obj, k_config, resolved_config);
              jsonv_obj_set(jsonv_arena, payload_obj, k_args, resolved_op_args);

              temp_step.is_http = false;
              temp_step.is_provider = false;
              temp_step.plugin.uses.data = plugin_name;
              temp_step.plugin.uses.length = strlen(plugin_name);
              temp_step.plugin.with_args = jsonv_val_obj(payload_obj);
              temp_step.plugin.sandboxed = false;

              int32_t status = plugin_start(&aj->plugin_exec, effective_arena, jsonv_arena, ast, *context_val, &temp_step);
              if (status != ERR_SUCCESS) {
                aj->job->execution_state = STATE_FAILED;
                return status;
              }
              break;
            }
          }
        }
      }

      ProviderConfigAST *pc = ast->providers_head;
      while (pc) {
        char *pc_name = sv_to_cstring(arena, pc->name);
        if (strcmp(pc_name, provider_name) == 0) {
          break;
        }
        pc = pc->next;
      }
      Jsonv_Value resolved_config = jsonv_val_undefined();
      if (pc) {
        resolved_config = resolve_json_value(arena, pc->config_val, jsonv_arena, *context_val);
      } else {
        resolved_config = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      }

      Jsonv_Value resolved_args = resolve_json_value(arena, step->prov.args, jsonv_arena, *context_val);

      Jsonv_Obj *payload_obj = jsonv_obj_new(jsonv_arena, NULL);
      char *k_op = allocate_jsonv_string(arena, "operation", 9);
      char *k_config = allocate_jsonv_string(arena, "configuration", 13);
      char *k_args = allocate_jsonv_string(arena, "args", 4);

      jsonv_obj_set(jsonv_arena, payload_obj, k_op, jsonv_val_str(allocate_jsonv_string(arena, operation, strlen(operation))));
      jsonv_obj_set(jsonv_arena, payload_obj, k_config, resolved_config);
      jsonv_obj_set(jsonv_arena, payload_obj, k_args, resolved_args);

      StepNode temp_step;
      memset(&temp_step, 0, sizeof(StepNode));
      temp_step.id = step->id;
      temp_step.is_http = false;
      temp_step.is_provider = false;
      temp_step.outputs_head = step->outputs_head;
      temp_step.plugin.uses.data = provider_name;
      temp_step.plugin.uses.length = strlen(provider_name);
      temp_step.plugin.with_args = jsonv_val_obj(payload_obj);
      temp_step.plugin.sandboxed = false;

      int32_t status = plugin_start(&aj->plugin_exec, effective_arena, jsonv_arena, ast, *context_val, &temp_step);
      if (status != ERR_SUCCESS) {
        aj->job->execution_state = STATE_FAILED;
        return status;
      }
      break;
    } else {
      int32_t status = plugin_start(&aj->plugin_exec, effective_arena, jsonv_arena, ast, *context_val, step);
      if (status != ERR_SUCCESS) {
        aj->job->execution_state = STATE_FAILED;
        return status;
      }
      break;
    }
  }

  if (aj->curr_step == NULL) {
    if (aj->is_loop) {
      aj->loop_iter++;
      int32_t status = start_loop_iteration(arena, jsonv_arena, context_val, aj);
      if (status != ERR_SUCCESS) {
        return status;
      }
      if (aj->curr_step != NULL && aj->job->execution_state != STATE_FAILED) {
        return advance_active_job(arena, jsonv_arena, ast, context_val, aj, transport);
      }
    } else {
      if (aj->job->execution_state != STATE_FAILED) {
        aj->job->execution_state = STATE_SUCCEEDED;

        // Add job steps outcomes to the global "jobs" object in the context
        char *job_id_cstr = allocate_jsonv_string(arena, aj->job->id.data, aj->job->id.length);
        Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->steps_state_obj);

        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
        }

        evaluate_job_variables(arena, jsonv_arena, context_val, aj->job, aj->local_vars);

        store_job_in_cache(arena, ast, *context_val, aj->job, aj);
      }
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

typedef struct IPCClient IPCClient;
struct IPCClient {
  int fd;
  char buf[1024];
  int len;
  IPCClient *next;
};

static void parse_cache_control(const char *cc, bool *no_cache, bool *no_store, int *max_age) {
  /*#region*/
  *no_cache = false;
  *no_store = false;
  *max_age = -1;
  if (!cc) return;

  char cc_copy[256];
  strncpy(cc_copy, cc, sizeof(cc_copy) - 1);
  cc_copy[sizeof(cc_copy) - 1] = '\0';

  char *token = strtok(cc_copy, ",");
  while (token) {
    while (*token == ' ' || *token == '\t') token++;
    char *end = token + strlen(token) - 1;
    while (end >= token && (*end == ' ' || *end == '\t')) *end-- = '\0';

    if (strcasecmp(token, "no-cache") == 0) {
      *no_cache = true;
    } else if (strcasecmp(token, "no-store") == 0) {
      *no_store = true;
    } else if (strncasecmp(token, "max-age=", 8) == 0) {
      *max_age = atoi(token + 8);
    }
    token = strtok(NULL, ",");
  }
  /*#endregion*/
}

static void generate_job_cache_key(Arena *arena, WorkflowAST *ast, Jsonv_Value context_val, JobNode *job, char *out_hex) {
  /*#region*/
  const char *job_type_str = (job->type == NODE_TASK) ? "task" : "transform";

  char *spec_json = "";
  Jsonv_Value jobs_val;
  if (jsonv_obj_get(ast->root_val.as.p, "jobs", &jobs_val) && jobs_val.tag == JSONV_VAL_OBJ) {
    char *job_id_cstr = sv_to_cstring(arena, job->id);
    Jsonv_Value job_spec_val;
    if (jsonv_obj_get(jobs_val.as.p, job_id_cstr, &job_spec_val)) {
      serialize_jsonv_value(arena, job_spec_val, &spec_json);
    }
  }

  char *inputs_json = "";
  Jsonv_Value inputs_val;
  if (jsonv_obj_get(context_val.as.p, "inputs", &inputs_val)) {
    serialize_jsonv_value(arena, inputs_val, &inputs_json);
  }

  char *env_json = "";
  Jsonv_Value env_val;
  if (jsonv_obj_get(context_val.as.p, "env", &env_val)) {
    serialize_jsonv_value(arena, env_val, &env_json);
  }

  cache_generate_key(job_type_str, spec_json, inputs_json, env_json, out_hex);
  /*#endregion*/
}

static bool job_has_resource(JobNode *job) {
  /*#region*/
  if (job->type == NODE_TASK) {
    StepNode *step = job->spec.task.steps_head;
    while (step) {
      if (step->is_resource) return true;
      step = step->next;
    }
  } else if (job->type == NODE_WAIT_SIGNAL) {
    StepNode *step = job->spec.wait_signal.steps_head;
    while (step) {
      if (step->is_resource) return true;
      step = step->next;
    }
  }
  return false;
  /*#endregion*/
}

static void store_job_in_cache(Arena *arena, WorkflowAST *ast, Jsonv_Value context_val, JobNode *job, ActiveJob *aj) {
  /*#region*/
  if (job_has_resource(job)) {
    return;
  }
  const char *no_cache_env = getenv("NESTOR_NO_CACHE");
  if (no_cache_env && (strcmp(no_cache_env, "true") == 0 || strcmp(no_cache_env, "1") == 0)) {
    return;
  }

  char cache_key[65];
  generate_job_cache_key(arena, ast, context_val, job, cache_key);

  char *job_type_str = (job->type == NODE_TASK) ? "task" : "transform";
  char *job_id_cstr = sv_to_cstring(arena, job->id);

  char *etag = NULL;
  char *last_modified = NULL;
  int ttl_seconds = -1;
  bool no_store = false;
  bool no_cache = false;

  if (job->type == NODE_TASK && aj) {
    StepNode *step = job->spec.task.steps_head;
    while (step) {
      if (step->is_http) {
        if (aj->resp_buf.cache_control[0] != '\0') {
          bool step_no_cache = false;
          bool step_no_store = false;
          int step_max_age = -1;
          parse_cache_control(aj->resp_buf.cache_control, &step_no_cache, &step_no_store, &step_max_age);
          if (step_no_store) no_store = true;
          if (step_no_cache) no_cache = true;
          if (step_max_age >= 0) {
            if (ttl_seconds == -1 || step_max_age < ttl_seconds) {
              ttl_seconds = step_max_age;
            }
          }
        }
        if (aj->resp_buf.expires[0] != '\0') {
          time_t exp_time = curl_getdate(aj->resp_buf.expires, NULL);
          if (exp_time != -1) {
            time_t now = time(NULL);
            int exp_ttl = (int)(exp_time - now);
            if (exp_ttl < 0) exp_ttl = 0;
            if (ttl_seconds == -1 || exp_ttl < ttl_seconds) {
              ttl_seconds = exp_ttl;
            }
          }
        }
        if (aj->resp_buf.etag[0] != '\0') {
          etag = aj->resp_buf.etag;
        }
        if (aj->resp_buf.last_modified[0] != '\0') {
          last_modified = aj->resp_buf.last_modified;
        }
      }
      step = step->next;
    }
  }

  if (no_store) {
    return;
  }

  if (no_cache && ttl_seconds > 0) {
    ttl_seconds = 0;
  }

  char *output_payload = NULL;
  if (job->type == NODE_TASK && aj) {
    serialize_jsonv_value(arena, aj->steps_state_obj, &output_payload);
  } else if (job->type == NODE_TRANSFORM) {
    Jsonv_Value jobs_val_obj;
    char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
    if (jsonv_obj_get(context_val.as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
      Jsonv_Value job_outcome_obj;
      if (jsonv_obj_get(jobs_val_obj.as.p, job_id_cstr, &job_outcome_obj) && job_outcome_obj.tag == JSONV_VAL_OBJ) {
        Jsonv_Value trans_outputs;
        char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
        if (jsonv_obj_get(job_outcome_obj.as.p, k_outputs, &trans_outputs)) {
          serialize_jsonv_value(arena, trans_outputs, &output_payload);
        }
      }
    }
  }

  if (output_payload) {
    char *headers_json = NULL;
    if (job->type == NODE_TASK && aj && aj->resp_buf.cache_control[0] != '\0') {
      Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast->jsonv_ctx);
      Jsonv_Obj *hdr_obj = jsonv_obj_new(jsonv_arena, NULL);
      char *k_cc = allocate_jsonv_string(arena, "Cache-Control", 13);
      jsonv_obj_set(jsonv_arena, hdr_obj, k_cc, jsonv_val_str(allocate_jsonv_string(arena, aj->resp_buf.cache_control, strlen(aj->resp_buf.cache_control))));
      if (etag) {
        char *k_etag = allocate_jsonv_string(arena, "ETag", 4);
        jsonv_obj_set(jsonv_arena, hdr_obj, k_etag, jsonv_val_str(allocate_jsonv_string(arena, etag, strlen(etag))));
      }
      if (last_modified) {
        char *k_lm = allocate_jsonv_string(arena, "Last-Modified", 13);
        jsonv_obj_set(jsonv_arena, hdr_obj, k_lm, jsonv_val_str(allocate_jsonv_string(arena, last_modified, strlen(last_modified))));
      }
      serialize_jsonv_value(arena, jsonv_val_obj(hdr_obj), &headers_json);
    }

    cache_store(cache_key, job_id_cstr, job_type_str, 200, headers_json, output_payload, etag, last_modified, ttl_seconds);
  }
  /*#endregion*/
}

static bool check_and_apply_cache(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, JobNode *job, ActiveJob *aj_out) {
  /*#region*/
  if (job_has_resource(job)) {
    return false;
  }
  const char *no_cache_env = getenv("NESTOR_NO_CACHE");
  if (no_cache_env && (strcmp(no_cache_env, "true") == 0 || strcmp(no_cache_env, "1") == 0)) {
    return false;
  }

  char cache_key[65];
  generate_job_cache_key(arena, ast, *context_val, job, cache_key);

  long status_code = 0;
  char *headers_json = NULL;
  char *output_payload = NULL;
  char *etag = NULL;
  char *last_modified = NULL;

  int32_t rc = cache_lookup(arena, cache_key, &status_code, &headers_json, &output_payload, &etag, &last_modified);
  if (rc == ERR_SUCCESS) {
    bool no_cache = false;
    bool no_store = false;
    int max_age = -1;

    if (headers_json) {
      Jsonv_Context *hdr_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
      Jsonv_Value hdr_val;
      if (hdr_ctx && jsonv_ctx_parse_data(hdr_ctx, (const unsigned char *)headers_json)) {
        jsonv_ctx_get_value(hdr_ctx, &hdr_val);
        Jsonv_Value cc_val;
        char *k_cc = allocate_jsonv_string(arena, "Cache-Control", 13);
        if (jsonv_obj_get(hdr_val.as.p, k_cc, &cc_val) && cc_val.tag == JSONV_VAL_STRING) {
          char *cc_str = sv_to_cstring(arena, (StringView){cc_val.as.p, jsonv_val_str_len(cc_val)});
          parse_cache_control(cc_str, &no_cache, &no_store, &max_age);
        }
      }
    }

    if (no_cache) {
      if (etag == NULL && last_modified == NULL) {
        return false;
      }
    }

    if (!no_cache) {
      if (job->type == NODE_TASK) {
        Jsonv_Value steps_obj_val = jsonv_val_undefined();
        Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
        if (temp_ctx && jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)output_payload)) {
          jsonv_ctx_get_value(temp_ctx, &steps_obj_val);
        }

        char *k_steps = allocate_jsonv_string(arena, "steps", 5);
        jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, steps_obj_val);

        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, steps_obj_val);

        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
        }
      } else if (job->type == NODE_TRANSFORM) {
        Jsonv_Value trans_val = jsonv_val_undefined();
        Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
        if (temp_ctx && jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)output_payload)) {
          jsonv_ctx_get_value(temp_ctx, &trans_val);
        }

        char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
        Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
        char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_outputs, trans_val);
        char *k_output = allocate_jsonv_string(arena, "output", 6);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_output, trans_val);
        char *k_result = allocate_jsonv_string(arena, "result", 6);
        jsonv_obj_set(jsonv_arena, job_outcome_obj, k_result, trans_val);

        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
        }
      }

      job->execution_state = STATE_SUCCEEDED;
      return true;
    } else if (aj_out) {
      aj_out->is_validating = true;
      strncpy(aj_out->cached_key, cache_key, 64);
      aj_out->cached_key[64] = '\0';
      if (etag) {
        strncpy(aj_out->cached_etag, etag, sizeof(aj_out->cached_etag) - 1);
        aj_out->cached_etag[sizeof(aj_out->cached_etag) - 1] = '\0';
      }
      if (last_modified) {
        strncpy(aj_out->cached_last_modified, last_modified, sizeof(aj_out->cached_last_modified) - 1);
        aj_out->cached_last_modified[sizeof(aj_out->cached_last_modified) - 1] = '\0';
      }
      aj_out->cached_output_payload = output_payload;
      return false;
    }
  }

  return false;
  /*#endregion*/
}

static bool jsonv_values_equal(Jsonv_Value a, Jsonv_Value b) {
  /*#region*/
  if (a.tag != b.tag) return false;
  switch (a.tag) {
    case JSONV_VAL_UNDEFINED:
    case JSONV_VAL_NULL:
      return true;
    case JSONV_VAL_BOOLEAN:
      return a.as.boolean == b.as.boolean;
    case JSONV_VAL_INT:
      return a.as.i == b.as.i;
    case JSONV_VAL_DOUBLE:
      return a.as.d == b.as.d;
    case JSONV_VAL_STRING: {
      size_t len_a = jsonv_val_str_len(a);
      size_t len_b = jsonv_val_str_len(b);
      if (len_a != len_b) return false;
      return strncmp(a.as.p, b.as.p, len_a) == 0;
    }
    default:
      return false;
  }
  /*#endregion*/
}

static void get_state_paths(WorkflowAST *ast, char *state_path, size_t state_len, char *lock_path, size_t lock_len) {
  /*#region*/
  char wf_name[256];
  if (ast->name.length > 0 && ast->name.length < 250) {
    memcpy(wf_name, ast->name.data, ast->name.length);
    wf_name[ast->name.length] = '\0';
  } else {
    strcpy(wf_name, "default");
  }
  snprintf(state_path, state_len, ".%s.tfstate", wf_name);
  snprintf(lock_path, lock_len, ".%s.tfstate.lock", wf_name);
  /*#endregion*/
}

static int32_t acquire_state_lock(const char *lock_path) {
  /*#region*/
  if (access(lock_path, F_OK) == 0) {
    return ERR_LOCKED;
  }
  FILE *f = fopen(lock_path, "w");
  if (!f) return ERR_OOM;
  fprintf(f, "%d", (int)getpid());
  fclose(f);
  return ERR_SUCCESS;
  /*#endregion*/
}

static void release_state_lock(const char *lock_path) {
  /*#region*/
  unlink(lock_path);
  /*#endregion*/
}

static int32_t save_tfstate(Arena *arena, const char *state_path, Jsonv_Value context_val) {
  /*#region*/
  char *serialized = NULL;
  int32_t rc = serialize_jsonv_value(arena, context_val, &serialized);
  if (rc != ERR_SUCCESS) return rc;
  FILE *f = fopen(state_path, "w");
  if (!f) return ERR_OOM;
  fputs(serialized, f);
  fclose(f);
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t load_tfstate(Arena *arena, Jsonv_Arena *jsonv_arena, const char *state_path, Jsonv_Value *out_context) {
  /*#region*/
  FILE *f = fopen(state_path, "r");
  if (!f) return ERR_MISSING_VAR;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = na_alloc(arena, len + 1);
  if (!buf) {
    fclose(f);
    return ERR_OOM;
  }
  size_t n = fread(buf, 1, len, f);
  buf[n] = '\0';
  fclose(f);

  Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
  if (!temp_ctx) return ERR_OOM;
  if (!jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)buf)) {
    return ERR_MISSING_VAR;
  }
  jsonv_ctx_get_value(temp_ctx, out_context);
  return ERR_SUCCESS;
  /*#endregion*/
}

static void merge_tfstate_into_context(Jsonv_Arena *jsonv_arena, Jsonv_Value tfstate_val, Jsonv_Value context_val) {
  /*#region*/
  if (tfstate_val.tag != JSONV_VAL_OBJ || context_val.tag != JSONV_VAL_OBJ) return;

  Jsonv_Value tf_jobs;
  if (jsonv_obj_get(tfstate_val.as.p, "jobs", &tf_jobs) && tf_jobs.tag == JSONV_VAL_OBJ) {
    Jsonv_Value ctx_jobs;
    if (jsonv_obj_get(context_val.as.p, "jobs", &ctx_jobs) && ctx_jobs.tag == JSONV_VAL_OBJ) {
      int len = jsonv_obj_length(tf_jobs.as.p);
      for (int i = 0; i < len; i++) {
        const char *key = jsonv_obj_key_at(tf_jobs.as.p, i);
        Jsonv_Value val = jsonv_obj_val_at(tf_jobs.as.p, i);
        jsonv_obj_set(jsonv_arena, ctx_jobs.as.p, key, val);
      }
    }
  }

  Jsonv_Value tf_provs;
  if (jsonv_obj_get(tfstate_val.as.p, "providers", &tf_provs) && tf_provs.tag == JSONV_VAL_OBJ) {
    Jsonv_Value ctx_provs;
    if (jsonv_obj_get(context_val.as.p, "providers", &ctx_provs) && ctx_provs.tag == JSONV_VAL_OBJ) {
      int len = jsonv_obj_length(tf_provs.as.p);
      for (int i = 0; i < len; i++) {
        const char *key = jsonv_obj_key_at(tf_provs.as.p, i);
        Jsonv_Value val = jsonv_obj_val_at(tf_provs.as.p, i);
        jsonv_obj_set(jsonv_arena, ctx_provs.as.p, key, val);
      }
    }
  }
  /*#endregion*/
}

static void resume_execution_state_from_context(WorkflowAST *ast, Jsonv_Value context_val) {
  /*#region*/
  JobNode *j_reset = ast->jobs_head;
  while (j_reset) {
    j_reset->execution_state = STATE_PENDING;
    j_reset = j_reset->next_sorted;
  }

  if (context_val.tag != JSONV_VAL_OBJ) return;
  Jsonv_Value jobs_val;
  if (!jsonv_obj_get(context_val.as.p, "jobs", &jobs_val) || jobs_val.tag != JSONV_VAL_OBJ) {
    return;
  }
  JobNode *job = ast->jobs_head;
  while (job) {
    Jsonv_Value entry;
    bool found = false;
    int len = jsonv_obj_length(jobs_val.as.p);
    for (int i = 0; i < len; i++) {
      const char *k = jsonv_obj_key_at(jobs_val.as.p, i);
      if (strlen(k) == job->id.length && strncmp(k, job->id.data, job->id.length) == 0) {
        entry = jsonv_obj_val_at(jobs_val.as.p, i);
        found = true;
        break;
      }
    }
    if (found && entry.tag == JSONV_VAL_OBJ) {
      Jsonv_Value status_val;
      if (jsonv_obj_get(entry.as.p, "status", &status_val) && status_val.tag == JSONV_VAL_STRING) {
        if (strcmp(status_val.as.p, "success") == 0) {
          job->execution_state = STATE_SUCCEEDED;
        } else if (strcmp(status_val.as.p, "failed") == 0) {
          job->execution_state = STATE_FAILED;
        } else if (strcmp(status_val.as.p, "skipped") == 0) {
          job->execution_state = STATE_SKIPPED;
        } else {
          job->execution_state = STATE_PENDING;
        }
      }
    }
    job = job->next_sorted;
  }
  /*#endregion*/
}

int32_t run_workflow_opt(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val, Transport *transport) {
  /*#region*/
  // Process the compiled graph concurrently using an event-driven scheduler.
  if (!arena || !ast || !context_val || !transport) return ERR_OOM;

  int32_t ret_val = ERR_SUCCESS;
  ActiveJob *active_jobs_head = NULL;

  ACNode *ac_root = ac_create_trie(arena, *context_val);
  set_global_ac_root(ac_root);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast->jsonv_ctx);

  if (!ast->providers_map) {
    WorkspaceMap *map = na_alloc(arena, sizeof(WorkspaceMap));
    if (map) {
      memset(map, 0, sizeof(WorkspaceMap));
      if (access("./providers", F_OK) == 0) {
        workspace_load_directory(arena, "./providers", map);
      }
      ast->providers_map = map;

    }
  }

  char state_path[270];
  char lock_path[270];
  get_state_paths(ast, state_path, sizeof(state_path), lock_path, sizeof(lock_path));

  int32_t lock_rc = acquire_state_lock(lock_path);
  if (lock_rc != ERR_SUCCESS) {
    fprintf(stderr, "Error: Execution lock active. Another Nestor instance is running.\n");
    return lock_rc;
  }

  Jsonv_Value tfstate_val = jsonv_val_undefined();
  if (load_tfstate(arena, jsonv_arena, state_path, &tfstate_val) == ERR_SUCCESS) {
    merge_tfstate_into_context(jsonv_arena, tfstate_val, *context_val);
    resume_execution_state_from_context(ast, *context_val);
  }

  VariableAST *rvar = ast->variables_head;
  while (rvar) {
    Jsonv_Value val = jsonv_val_undefined();
    int32_t status = evaluate_expression(arena, rvar->expression, jsonv_arena, *context_val, &val);
    if (status == ERR_SUCCESS) {
      char *name_cstr = allocate_jsonv_string(arena, rvar->name.data, rvar->name.length);
      jsonv_obj_set(jsonv_arena, context_val->as.p, name_cstr, val);
    }
    rvar = rvar->next;
  }

  if (ast->providers_head) {
    Jsonv_Obj *providers_obj = jsonv_obj_new(jsonv_arena, NULL);
    ProviderConfigAST *pc = ast->providers_head;
    while (pc) {
      Jsonv_Value resolved_pc = resolve_json_value(arena, pc->config_val, jsonv_arena, *context_val);
      char *pc_name = sv_to_cstring(arena, pc->name);
      jsonv_obj_set(jsonv_arena, providers_obj, pc_name, resolved_pc);
      pc = pc->next;
    }
    char *k_providers = allocate_jsonv_string(arena, "providers", 9);
    jsonv_obj_set(jsonv_arena, context_val->as.p, k_providers, jsonv_val_obj(providers_obj));
  }

  Jsonv_Obj *jobs_root_obj = jsonv_obj_new(jsonv_arena, NULL);
  char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
  jsonv_obj_set(jsonv_arena, context_val->as.p, k_jobs, jsonv_val_obj(jobs_root_obj));

  ast->ipc_socket_path[0] = '\0';
  int ipc_listen_fd = -1;
  IPCClient *ipc_clients_head = NULL;

  snprintf(ast->ipc_socket_path, sizeof(ast->ipc_socket_path), "/tmp/nestor_ipc_%p.sock", (void *)ast);
  unlink(ast->ipc_socket_path);

  ipc_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ipc_listen_fd >= 0) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ast->ipc_socket_path, sizeof(addr.sun_path) - 1);
    if (bind(ipc_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        listen(ipc_listen_fd, 5) == 0) {
      int flags = fcntl(ipc_listen_fd, F_GETFL, 0);
      fcntl(ipc_listen_fd, F_SETFL, flags | O_NONBLOCK);
    } else {
      close(ipc_listen_fd);
      ipc_listen_fd = -1;
      ast->ipc_socket_path[0] = '\0';
    }
  }

  while (1) {
    if (ipc_listen_fd >= 0) {
      while (1) {
        int client_fd = accept(ipc_listen_fd, NULL, NULL);
        if (client_fd < 0) break;
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        IPCClient *cli = na_alloc(arena, sizeof(IPCClient));
        if (cli) {
          cli->fd = client_fd;
          cli->len = 0;
          cli->buf[0] = '\0';
          cli->next = ipc_clients_head;
          ipc_clients_head = cli;
        } else {
          close(client_fd);
        }
      }
    }

    IPCClient **curr_cli = &ipc_clients_head;
    while (*curr_cli) {
      IPCClient *cli = *curr_cli;
      char read_buf[256];
      ssize_t n = recv(cli->fd, read_buf, sizeof(read_buf) - 1, 0);
      if (n > 0) {
        if (cli->len + n < (int)sizeof(cli->buf) - 1) {
          memcpy(cli->buf + cli->len, read_buf, n);
          cli->len += n;
          cli->buf[cli->len] = '\0';
        }
        char *newline = strchr(cli->buf, '\n');
        if (newline) {
          *newline = '\0';
          process_ipc_request(arena, jsonv_arena, context_val, cli->fd, cli->buf);
          close(cli->fd);
          *curr_cli = cli->next;
          continue;
        }
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(cli->fd);
        *curr_cli = cli->next;
        continue;
      }
      curr_cli = &(cli->next);
    }

    update_job_states(ast);

    bool control_flow_progress = true;
    while (control_flow_progress) {
      control_flow_progress = false;
      update_job_states(ast);

      JobNode *job = ast->jobs_head;
      while (job) {
        if (job->execution_state == STATE_PENDING && is_ready(job)) {
          bool job_is_res = job_has_resource(job);
          if (job_is_res) {
            bool active_running = false;
            ActiveJob *curr_aj = active_jobs_head;
            while (curr_aj) {
              if (curr_aj->job->execution_state == STATE_RUNNING) {
                active_running = true;
                break;
              }
              curr_aj = curr_aj->next;
            }
            if (active_running) {
              job = job->next_sorted;
              continue;
            }
          } else {
            bool resource_running = false;
            ActiveJob *curr_aj = active_jobs_head;
            while (curr_aj) {
              if (curr_aj->job->execution_state == STATE_RUNNING && job_has_resource(curr_aj->job)) {
                resource_running = true;
                break;
              }
              curr_aj = curr_aj->next;
            }
            if (resource_running) {
              job = job->next_sorted;
              continue;
            }
          }
          if (job->type == NODE_IF) {
            job->execution_state = STATE_RUNNING;
            Jsonv_Value eval_res = jsonv_val_undefined();
            int32_t status = evaluate_expression(arena, job->spec.binary_if.condition, jsonv_arena, *context_val, &eval_res);
            if (status != ERR_SUCCESS) {
              job->execution_state = STATE_FAILED;
              ret_val = status;
              goto cleanup;
            }

            if (is_truthy(eval_res)) {
              job->execution_state = STATE_SUCCEEDED;
              for (size_t c = 0; c < job->spec.binary_if.else_count; c++) {
                mark_job_skipped(ast, job->spec.binary_if.else_branch[c]);
              }
            } else {
              job->execution_state = STATE_SUCCEEDED;
              for (size_t c = 0; c < job->spec.binary_if.then_count; c++) {
                mark_job_skipped(ast, job->spec.binary_if.then_branch[c]);
              }
            }
            control_flow_progress = true;
            break;
          } else if (job->type == NODE_SWITCH) {
            job->execution_state = STATE_RUNNING;
            SwitchCase *sc = job->spec.multi_switch.cases;
            bool matched = false;
            while (sc) {
              if (!matched) {
                Jsonv_Value eval_res = jsonv_val_undefined();
                int32_t status = evaluate_expression(arena, sc->condition, jsonv_arena, *context_val, &eval_res);
                if (status == ERR_SUCCESS && is_truthy(eval_res)) {
                  matched = true;
                } else {
                  for (size_t t = 0; t < sc->then_count; t++) {
                    mark_job_skipped(ast, sc->then_branch[t]);
                  }
                }
              } else {
                for (size_t t = 0; t < sc->then_count; t++) {
                  mark_job_skipped(ast, sc->then_branch[t]);
                }
              }
              sc = sc->next;
            }
            if (matched) {
              for (size_t d = 0; d < job->spec.multi_switch.default_count; d++) {
                mark_job_skipped(ast, job->spec.multi_switch.default_branch[d]);
              }
            }
            job->execution_state = STATE_SUCCEEDED;
            control_flow_progress = true;
            break;
          } else if (job->type == NODE_FORK) {
            job->execution_state = STATE_SUCCEEDED;
            control_flow_progress = true;
            break;
          } else if (job->type == NODE_WAIT_SIGNAL) {
            Jsonv_Value resolved_corr = jsonv_val_undefined();
            int32_t status = evaluate_expression(arena, job->spec.wait_signal.correlation_id, jsonv_arena, *context_val, &resolved_corr);
            if (status != ERR_SUCCESS) {
              job->execution_state = STATE_FAILED;
              ret_val = status;
              goto cleanup;
            }

            Jsonv_Value incoming_corr = jsonv_val_undefined();
            bool incoming_matched = false;
            Jsonv_Value inputs_val;
            if (jsonv_obj_get(context_val->as.p, "inputs", &inputs_val) && inputs_val.tag == JSONV_VAL_OBJ) {
              if (jsonv_obj_get(inputs_val.as.p, "correlation_id", &incoming_corr)) {
                if (jsonv_values_equal(resolved_corr, incoming_corr)) {
                  incoming_matched = true;
                }
              }
            }

            if (incoming_matched) {
              if (job->spec.wait_signal.steps_head) {
                ActiveJob *aj = na_alloc(arena, sizeof(ActiveJob));
                if (!aj) {
                  ret_val = ERR_OOM;
                  goto cleanup;
                }
                memset(aj, 0, sizeof(ActiveJob));
                aj->local_vars.tag = JSONV_VAL_OBJ;
                aj->local_vars.as.p = jsonv_obj_new(jsonv_arena, NULL);
                aj->job = job;

                aj->steps_state_obj.tag = JSONV_VAL_OBJ;
                aj->steps_state_obj.as.p = jsonv_obj_new(jsonv_arena, NULL);
                char *k_steps = allocate_jsonv_string(arena, "steps", 5);
                jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

                char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
                Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
                jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->steps_state_obj);
                Jsonv_Value jobs_val_obj;
                char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
                if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
                  jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
                }

                StepNode *step = job->spec.wait_signal.steps_head;
                Jsonv_Value jobs_obj;
                if (jsonv_obj_get(context_val->as.p, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ) {
                  Jsonv_Value job_entry;
                  if (jsonv_obj_get(jobs_obj.as.p, job_id_cstr, &job_entry) && job_entry.tag == JSONV_VAL_OBJ) {
                    Jsonv_Value steps_obj;
                    if (jsonv_obj_get(job_entry.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ) {
                      int slen = jsonv_obj_length(steps_obj.as.p);
                      for (int s = 0; s < slen; s++) {
                        const char *skey = jsonv_obj_key_at(steps_obj.as.p, s);
                        Jsonv_Value sval = jsonv_obj_val_at(steps_obj.as.p, s);
                        jsonv_obj_set(jsonv_arena, aj->steps_state_obj.as.p, skey, sval);
                      }
                      while (step) {
                        char *step_id_cstr = sv_to_cstring(arena, step->id);
                        Jsonv_Value dummy;
                        if (!jsonv_obj_get(steps_obj.as.p, step_id_cstr, &dummy)) {
                          break;
                        }
                        step = step->next;
                      }
                    }
                  }
                }

                aj->curr_step = step;
                aj->is_loop = false;

                if (step == NULL) {
                  job->execution_state = STATE_SUCCEEDED;
                  char *k_status = allocate_jsonv_string(arena, "status", 6);
                  jsonv_obj_set(jsonv_arena, job_outcome_obj, k_status, jsonv_val_str(allocate_jsonv_string(arena, "success", 7)));
                  control_flow_progress = true;
                } else {
                  job->execution_state = STATE_RUNNING;
                  aj->next = active_jobs_head;
                  active_jobs_head = aj;
                  push_local_vars(jsonv_arena, *context_val, aj->local_vars);
                  evaluate_job_variables(arena, jsonv_arena, context_val, aj->job, aj->local_vars);
                  int32_t status = advance_active_job(arena, jsonv_arena, ast, context_val, aj, transport);
                  pop_local_vars(jsonv_arena, *context_val, aj->local_vars);
                  if (status != ERR_SUCCESS) {
                    ret_val = status;
                    goto cleanup;
                  }
                }
              } else {
                char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
                Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
                char *k_status = allocate_jsonv_string(arena, "status", 6);
                jsonv_obj_set(jsonv_arena, job_outcome_obj, k_status, jsonv_val_str(allocate_jsonv_string(arena, "success", 7)));
                Jsonv_Value jobs_val_obj;
                char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
                if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
                  jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
                }
                job->execution_state = STATE_SUCCEEDED;
                control_flow_progress = true;
              }
            } else {
              job->execution_state = STATE_SUSPENDED;
              fprintf(stderr, "Workflow suspended at wait_signal job '%.*s'.\n", (int)job->id.length, job->id.data);
              save_tfstate(arena, state_path, *context_val);
              control_flow_progress = false;
              ret_val = ERR_SUCCESS;
              break;
            }
            break;
          } else if (job->type == NODE_TRANSFORM) {
            if (check_and_apply_cache(arena, jsonv_arena, ast, context_val, job, NULL)) {
              control_flow_progress = true;
              break;
            }
            job->execution_state = STATE_RUNNING;

            Jsonv_Value local_vars;
            local_vars.tag = JSONV_VAL_OBJ;
            local_vars.as.p = jsonv_obj_new(jsonv_arena, NULL);
            push_local_vars(jsonv_arena, *context_val, local_vars);
            evaluate_job_variables(arena, jsonv_arena, context_val, job, local_vars);

            Jsonv_Value transform_res = jsonv_val_undefined();
            int32_t status = evaluate_expression(arena, job->spec.transform.expression, jsonv_arena, *context_val, &transform_res);
            if (status != ERR_SUCCESS) {
              pop_local_vars(jsonv_arena, *context_val, local_vars);
              job->execution_state = STATE_FAILED;
              ret_val = status;
              goto cleanup;
            }

            char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
            Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
            char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
            jsonv_obj_set(jsonv_arena, job_outcome_obj, k_outputs, transform_res);
            char *k_output = allocate_jsonv_string(arena, "output", 6);
            jsonv_obj_set(jsonv_arena, job_outcome_obj, k_output, transform_res);
            char *k_result = allocate_jsonv_string(arena, "result", 6);
            jsonv_obj_set(jsonv_arena, job_outcome_obj, k_result, transform_res);

            Jsonv_Value jobs_val_obj;
            char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
            if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
              jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
            }

            evaluate_job_variables(arena, jsonv_arena, context_val, job, local_vars);

            pop_local_vars(jsonv_arena, *context_val, local_vars);

            job->execution_state = STATE_SUCCEEDED;
            store_job_in_cache(arena, ast, *context_val, job, NULL);
            control_flow_progress = true;
            break;
          }
        }
        job = job->next_sorted;
      }
    }

    // Check if any exit/end job has succeeded
    JobNode *exit_job = NULL;
    JobNode *jc = ast->jobs_head;
    while (jc) {
      if (jc->execution_state == STATE_SUCCEEDED && (jc->is_end || jc->return_expr.length > 0)) {
        exit_job = jc;
        break;
      }
      jc = jc->next_sorted;
    }

    if (exit_job) {
      Jsonv_Value evaluated_out = jsonv_val_undefined();
      if (exit_job->return_expr.length > 0) {
        StringView expr = exit_job->return_expr;
        // Trim leading whitespace
        while (expr.length > 0 && (expr.data[0] == ' ' || expr.data[0] == '\t' || expr.data[0] == '\n' || expr.data[0] == '\r')) {
          expr.data++;
          expr.length--;
        }
        if (expr.length > 0 && (expr.data[0] == '{' || expr.data[0] == '[')) {
          // Parse JSON structure
          char *json_cstr = na_alloc(arena, expr.length + 1);
          if (json_cstr) {
            memcpy(json_cstr, expr.data, expr.length);
            json_cstr[expr.length] = '\0';

            Jsonv_Config config = {0};
            config.default_block_size = 4096;
            config.max_limit = 16 * 1024 * 1024;
            config.max_depth = 128;
            config.max_values = 10000;
            config.max_objects = 5000;
            config.max_array = 5000;
            config.max_string_bytes = 4 * 1024 * 1024;

            Jsonv_Arena_Error parser_err = 0;
            Jsonv_Context *tmp_ctx = jsonv_ctx_new(jsonv_arena, &config, &parser_err);
            if (tmp_ctx) {
              if (jsonv_ctx_parse_yaml_data(tmp_ctx, (const unsigned char *)json_cstr) ||
                  jsonv_ctx_parse_data(tmp_ctx, (const unsigned char *)json_cstr)) {
                Jsonv_Value parsed_val;
                if (jsonv_ctx_get_value(tmp_ctx, &parsed_val)) {
                  evaluated_out = resolve_json_value(arena, parsed_val, jsonv_arena, *context_val);
                }
              }
            }
          }
        } else {
          int32_t status = evaluate_expression(arena, expr, jsonv_arena, *context_val, &evaluated_out);
          if (status != ERR_SUCCESS) {
            ret_val = status;
            goto cleanup;
          }
        }
      } else {
        // "return" is not defined, use job outputs
        Jsonv_Value jobs_val_obj;
        char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
        if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
          char *job_id_cstr = allocate_jsonv_string(arena, exit_job->id.data, exit_job->id.length);
          Jsonv_Value job_outcome_val;
          if (jsonv_obj_get(jobs_val_obj.as.p, job_id_cstr, &job_outcome_val) && job_outcome_val.tag == JSONV_VAL_OBJ) {
            char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
            Jsonv_Value job_outputs_val;
            if (jsonv_obj_get(job_outcome_val.as.p, k_outputs, &job_outputs_val)) {
              evaluated_out = job_outputs_val;
            }
          }
        }
      }

      // Bind evaluated value to a top-level "outputs" key
      char *k_outputs = allocate_jsonv_string(arena, "outputs", 7);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_outputs, evaluated_out);

      ret_val = ERR_SUCCESS;
      goto cleanup;
    }

    JobNode *job = ast->jobs_head;
    while (job) {
      if (job->execution_state == STATE_PENDING && is_ready(job)) {
        if (ast->max_concurrency > 0) {
          int running_jobs = 0;
          ActiveJob *curr_aj = active_jobs_head;
          while (curr_aj) {
            running_jobs++;
            curr_aj = curr_aj->next;
          }
          if (running_jobs >= ast->max_concurrency) {
            break;
          }
        }
        ActiveJob temp_aj;
        memset(&temp_aj, 0, sizeof(ActiveJob));
        if (job->type == NODE_TASK) {
          if (check_and_apply_cache(arena, jsonv_arena, ast, context_val, job, &temp_aj)) {
            control_flow_progress = true;
            break;
          }
        }

        if (job->type == NODE_TASK || job->type == NODE_LOOP || job->type == NODE_WAIT_TIMER) {
          ActiveJob *aj = na_alloc(arena, sizeof(ActiveJob));
          if (!aj) {
            ret_val = ERR_OOM;
            goto cleanup;
          }
          memset(aj, 0, sizeof(ActiveJob));
          aj->local_vars.tag = JSONV_VAL_OBJ;
          aj->local_vars.as.p = jsonv_obj_new(jsonv_arena, NULL);
          aj->job = job;

          if (job->type == NODE_TASK) {
            if (temp_aj.is_validating) {
              aj->is_validating = true;
              strcpy(aj->cached_key, temp_aj.cached_key);
              strcpy(aj->cached_etag, temp_aj.cached_etag);
              strcpy(aj->cached_last_modified, temp_aj.cached_last_modified);
              aj->cached_output_payload = temp_aj.cached_output_payload;

              if (aj->cached_etag[0] != '\0') {
                char *k_cache_etag = allocate_jsonv_string(arena, "_cache_etag", 11);
                jsonv_obj_set(jsonv_arena, context_val->as.p, k_cache_etag, jsonv_val_str(allocate_jsonv_string(arena, aj->cached_etag, strlen(aj->cached_etag))));
              }
              if (aj->cached_last_modified[0] != '\0') {
                char *k_cache_lm = allocate_jsonv_string(arena, "_cache_last_modified", 20);
                jsonv_obj_set(jsonv_arena, context_val->as.p, k_cache_lm, jsonv_val_str(allocate_jsonv_string(arena, aj->cached_last_modified, strlen(aj->cached_last_modified))));
              }
            }

             aj->steps_state_obj.tag = JSONV_VAL_OBJ;
             aj->steps_state_obj.as.p = jsonv_obj_new(jsonv_arena, NULL);
             char *k_steps = allocate_jsonv_string(arena, "steps", 5);
             jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);

             char *job_id_cstr = allocate_jsonv_string(arena, job->id.data, job->id.length);
             Jsonv_Obj *job_outcome_obj = jsonv_obj_new(jsonv_arena, NULL);
             jsonv_obj_set(jsonv_arena, job_outcome_obj, k_steps, aj->steps_state_obj);
             Jsonv_Value jobs_val_obj;
             char *k_jobs = allocate_jsonv_string(arena, "jobs", 4);
             if (jsonv_obj_get(context_val->as.p, k_jobs, &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
               jsonv_obj_set(jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
             }

            StepNode *step = job->spec.task.steps_head;
            Jsonv_Value jobs_obj;
            if (jsonv_obj_get(context_val->as.p, "jobs", &jobs_obj) && jobs_obj.tag == JSONV_VAL_OBJ) {
              Jsonv_Value job_entry;
              char *job_id_cstr = sv_to_cstring(arena, job->id);
              if (jsonv_obj_get(jobs_obj.as.p, job_id_cstr, &job_entry) && job_entry.tag == JSONV_VAL_OBJ) {
                Jsonv_Value steps_obj;
                if (jsonv_obj_get(job_entry.as.p, "steps", &steps_obj) && steps_obj.tag == JSONV_VAL_OBJ) {
                  int slen = jsonv_obj_length(steps_obj.as.p);
                  for (int s = 0; s < slen; s++) {
                    const char *skey = jsonv_obj_key_at(steps_obj.as.p, s);
                    Jsonv_Value sval = jsonv_obj_val_at(steps_obj.as.p, s);
                    jsonv_obj_set(jsonv_arena, aj->steps_state_obj.as.p, skey, sval);
                  }
                  while (step) {
                    char *step_id_cstr = sv_to_cstring(arena, step->id);
                    Jsonv_Value dummy;
                    if (!jsonv_obj_get(steps_obj.as.p, step_id_cstr, &dummy)) {
                      break;
                    }
                    step = step->next;
                  }
                }
              }
            }
            aj->curr_step = step;
            aj->is_loop = false;
          } else if (job->type == NODE_LOOP) {
            aj->is_loop = true;
            aj->max_iterations = job->spec.loop_node.max_iterations;
            if (aj->max_iterations == 0) {
              aj->max_iterations = 1000;
            }
            aj->loop_iter = 0;
            int32_t status = start_loop_iteration(arena, jsonv_arena, context_val, aj);
            if (status != ERR_SUCCESS) {
              ret_val = status;
              goto cleanup;
            }
          } else if (job->type == NODE_WAIT_TIMER) {
            aj->is_loop = false;
            StringView resolved_dur;
            resolve_string(arena, job->spec.wait_timer.duration, jsonv_arena, *context_val, &resolved_dur);
            long duration_ms = parse_duration_ms(resolved_dur);
            struct timeval now;
            gettimeofday(&now, NULL);
            long sec = duration_ms / 1000;
            long usec = (duration_ms % 1000) * 1000;
            aj->next_retry_time.tv_sec = now.tv_sec + sec;
            aj->next_retry_time.tv_usec = now.tv_usec + usec;
            if (aj->next_retry_time.tv_usec >= 1000000) {
              aj->next_retry_time.tv_sec++;
              aj->next_retry_time.tv_usec -= 1000000;
            }
            aj->is_waiting_retry = true;
          }

          job->execution_state = STATE_RUNNING;
          push_local_vars(jsonv_arena, *context_val, aj->local_vars);
          evaluate_job_variables(arena, jsonv_arena, context_val, aj->job, aj->local_vars);
          int32_t status = advance_active_job(arena, jsonv_arena, ast, context_val, aj, transport);
          pop_local_vars(jsonv_arena, *context_val, aj->local_vars);
          if (status != ERR_SUCCESS) {
            ret_val = status;
            goto cleanup;
          }

          if (job->execution_state == STATE_RUNNING) {
            aj->next = active_jobs_head;
            active_jobs_head = aj;
          }
        } else if (job->type == NODE_IF || job->type == NODE_SWITCH || job->type == NODE_FORK ||
                   job->type == NODE_JOIN || job->type == NODE_WAIT_SIGNAL || job->type == NODE_TRANSFORM) {
          job = job->next_sorted;
          continue;
        } else {
          job->execution_state = STATE_FAILED;
          ret_val = ERR_CLI_UNSUPPORTED_BLOCKING_NODE;
          goto cleanup;
        }
      }
      job = job->next_sorted;
    }

    ActiveJob **curr_ptr = &active_jobs_head;
    while (*curr_ptr) {
      ActiveJob *aj = *curr_ptr;
      if (aj->job->execution_state != STATE_RUNNING) {
        if (aj->loop_arena) {
          arena_destroy(aj->loop_arena);
          aj->loop_arena = NULL;
        }
        *curr_ptr = aj->next;
      } else {
        curr_ptr = &(aj->next);
      }
    }

    bool active_remaining = false;
    if (active_jobs_head != NULL) {
      active_remaining = true;
    } else {
      JobNode *j = ast->jobs_head;
      while (j) {
        if (j->execution_state == STATE_PENDING) {
          active_remaining = true;
          break;
        }
        j = j->next_sorted;
      }
    }

    if (!active_remaining) break;

    // Tick and trigger scheduled retries
    ActiveJob *aj_retry = active_jobs_head;
    while (aj_retry) {
      if (aj_retry->is_waiting_retry) {
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > aj_retry->next_retry_time.tv_sec ||
            (now.tv_sec == aj_retry->next_retry_time.tv_sec && now.tv_usec >= aj_retry->next_retry_time.tv_usec)) {
          aj_retry->is_waiting_retry = false;
          push_local_vars(jsonv_arena, *context_val, aj_retry->local_vars);
          int32_t status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_retry, transport);
          pop_local_vars(jsonv_arena, *context_val, aj_retry->local_vars);
          if (status != ERR_SUCCESS) {
            ret_val = status;
            goto cleanup;
          }
        }
      }
      aj_retry = aj_retry->next;
    }

    int still_running = 0;
    transport->ops->poll_requests(transport, &still_running);

    ActiveJob *aj_http = active_jobs_head;
    while (aj_http) {
      if (aj_http->easy_handle) {
        push_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
        long status_code = 0;
        bool completed = false;
        bool error = false;
        int32_t status = transport->ops->check_completed(transport, arena, jsonv_arena, aj_http->easy_handle, &aj_http->resp_buf, &status_code, &completed, &error);
        if (status == ERR_SUCCESS && completed) {
          if (!error) {
            Arena *eff_arena = aj_http->loop_arena ? aj_http->loop_arena : arena;
            if (status_code >= 400 && handle_step_failure(eff_arena, jsonv_arena, context_val, aj_http, "HTTP status >= 400")) {
              transport->ops->cleanup_request(transport, aj_http->easy_handle);
              aj_http->easy_handle = NULL;
              pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
              aj_http = aj_http->next;
              continue;
            }
            if (status_code >= 400 && aj_http->curr_step->has_on_error) {
              transport->ops->cleanup_request(transport, aj_http->easy_handle);
              aj_http->easy_handle = NULL;
              int32_t comp_status = apply_step_fallback(arena, jsonv_arena, context_val, aj_http, ast, transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
                ret_val = comp_status;
                goto cleanup;
              }
              pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
              aj_http = aj_http->next;
              continue;
            }
            int32_t comp_status = complete_http_step_async(arena, jsonv_arena, aj_http, status_code);
            if (comp_status != ERR_SUCCESS) {
              pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
              ret_val = comp_status;
              goto cleanup;
            }
            if (status_code >= 400) {
              aj_http->job->execution_state = STATE_FAILED;
            }
          } else {
            Arena *eff_arena = aj_http->loop_arena ? aj_http->loop_arena : arena;
            if (handle_step_failure(eff_arena, jsonv_arena, context_val, aj_http, "HTTP transport error")) {
              transport->ops->cleanup_request(transport, aj_http->easy_handle);
              aj_http->easy_handle = NULL;
              pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
              aj_http = aj_http->next;
              continue;
            }
            if (aj_http->curr_step->has_on_error) {
              transport->ops->cleanup_request(transport, aj_http->easy_handle);
              aj_http->easy_handle = NULL;
              int32_t comp_status = apply_step_fallback(arena, jsonv_arena, context_val, aj_http, ast, transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
                ret_val = comp_status;
                goto cleanup;
              }
              pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
              aj_http = aj_http->next;
              continue;
            }
            aj_http->job->execution_state = STATE_FAILED;
          }

          transport->ops->cleanup_request(transport, aj_http->easy_handle);
          aj_http->easy_handle = NULL;
          aj_http->curr_step_retry_attempt = 0;

          // Re-evaluate job variables since step outcomes changed
          evaluate_job_variables(arena, jsonv_arena, context_val, aj_http->job, aj_http->local_vars);

          aj_http->curr_step = aj_http->curr_step->next;

          int32_t adv_status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_http, transport);
          if (adv_status != ERR_SUCCESS) {
            pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
            ret_val = adv_status;
            goto cleanup;
          }
        }
        pop_local_vars(jsonv_arena, *context_val, aj_http->local_vars);
      }
      aj_http = aj_http->next;
    }

    // Poll and read from active child processes (plugins)
    ActiveJob *aj_proc = active_jobs_head;
    while (aj_proc) {
      if (aj_proc->plugin_exec.child_pid != 0) {
        push_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
        bool finished = false;
        long exit_code = 0;
        Arena *eff_arena = aj_proc->loop_arena ? aj_proc->loop_arena : arena;

        plugin_poll(&aj_proc->plugin_exec, eff_arena, jsonv_arena, context_val, aj_proc->curr_step, &finished, &exit_code);
        if (finished) {
          if (exit_code == -4) { // Timeout
            if (handle_step_failure(eff_arena, jsonv_arena, context_val, aj_proc, "Plugin timeout")) {
              pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
              aj_proc = aj_proc->next;
              continue;
            }
            if (aj_proc->curr_step->has_on_error) {
              int32_t comp_status = apply_step_fallback(arena, jsonv_arena, context_val, aj_proc, ast, transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
                ret_val = comp_status;
                goto cleanup;
              }
              pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
              aj_proc = aj_proc->next;
              continue;
            }
            complete_plugin_step_async(arena, jsonv_arena, aj_proc, -4);
            aj_proc->job->execution_state = STATE_FAILED;
          } else { // Exited
            if (exit_code != 0 && handle_step_failure(eff_arena, jsonv_arena, context_val, aj_proc, "Plugin exited with non-zero code")) {
              pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
              aj_proc = aj_proc->next;
              continue;
            }
            if (exit_code != 0 && aj_proc->curr_step->has_on_error) {
              int32_t comp_status = apply_step_fallback(arena, jsonv_arena, context_val, aj_proc, ast, transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
                ret_val = comp_status;
                goto cleanup;
              }
              pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
              aj_proc = aj_proc->next;
              continue;
            }
            complete_plugin_step_async(arena, jsonv_arena, aj_proc, exit_code);
            aj_proc->curr_step_retry_attempt = 0;

            evaluate_job_variables(arena, jsonv_arena, context_val, aj_proc->job, aj_proc->local_vars);

            aj_proc->curr_step = aj_proc->curr_step->next;

            int32_t adv_status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_proc, transport);
            if (adv_status != ERR_SUCCESS) {
              pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
              ret_val = adv_status;
              goto cleanup;
            }
          }
        }
        pop_local_vars(jsonv_arena, *context_val, aj_proc->local_vars);
      }
      aj_proc = aj_proc->next;
    }

    usleep(1000);
  }

  JobNode *j = ast->jobs_head;
  while (j) {
    if (j->execution_state == STATE_FAILED) {
      ret_val = ERR_HTTP_TRANSPORT;
      goto cleanup;
    }
    j = j->next_sorted;
  }

  ret_val = ERR_SUCCESS;

cleanup:
  set_global_ac_root(NULL);
  ActiveJob *aj_cleanup = active_jobs_head;
  while (aj_cleanup) {
    if (aj_cleanup->loop_arena) {
      arena_destroy(aj_cleanup->loop_arena);
      aj_cleanup->loop_arena = NULL;
    }
    if (aj_cleanup->easy_handle) {
      transport->ops->cleanup_request(transport, aj_cleanup->easy_handle);
    }
    plugin_cleanup(&aj_cleanup->plugin_exec);
    aj_cleanup = aj_cleanup->next;
  }
  IPCClient *cli_c = ipc_clients_head;
  while (cli_c) {
    close(cli_c->fd);
    cli_c = cli_c->next;
  }
  if (ipc_listen_fd >= 0) {
    close(ipc_listen_fd);
  }
  if (ast->ipc_socket_path[0] != '\0') {
    unlink(ast->ipc_socket_path);
  }
  bool has_suspended = false;
  JobNode *js = ast->jobs_head;
  while (js) {
    if (js->execution_state == STATE_SUSPENDED) {
      has_suspended = true;
      break;
    }
    js = js->next_sorted;
  }

  if (ret_val == ERR_SUCCESS && !has_suspended) {
    unlink(state_path);
  } else {
    save_tfstate(arena, state_path, *context_val);
  }
  release_state_lock(lock_path);

  return ret_val;
  /*#endregion*/
}

int32_t run_workflow(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val) {
  /*#region*/
  Transport *transport = transport_curl_new(arena);
  if (!transport) return ERR_HTTP_TRANSPORT;
  int32_t res = run_workflow_opt(arena, ast, context_val, transport);
  transport->ops->destroy(transport);
  return res;
  /*#endregion*/
}
