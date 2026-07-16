#include "runner.h"
#include "evaluator.h"
#include "aho_corasick.h"
#include "transport.h"
#include "plugin.h"
#include <curl/curl.h>
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
#include <errno.h>

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

    // 6. Capture response body
    ResponseBuffer resp_buf = { arena, NULL, 0, 0 };
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

      ResponseBuffer resp_buf = { arena, NULL, 0, 0 };
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

  // Loop support
  bool is_loop;
  size_t loop_iter;
  size_t max_iterations;

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

  ActiveJob *next;
};

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
          }

          if (job->type == NODE_JOIN) {
            bool join_ok = false;
            bool join_evaluated = false;

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
      job = job->next_sorted;
    }
  }
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
      return ERR_SUCCESS;
    }

    aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
    char *k_steps = allocate_jsonv_string(arena, "steps", 5);
    jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
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

      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
      aj->curr_step = job->spec.loop_node.steps_head;
    } else {
      if (aj->loop_iter > 0) {
        job->execution_state = STATE_SUCCEEDED;
        aj->curr_step = NULL;
        return ERR_SUCCESS;
      }

      char *k_index = allocate_jsonv_string(arena, "index", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_index, jsonv_val_int(0));

      char *k_item = allocate_jsonv_string(arena, "item", 4);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_item, items_val);

      aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
      char *k_steps = allocate_jsonv_string(arena, "steps", 5);
      jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
      aj->curr_step = job->spec.loop_node.steps_head;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}


static int32_t complete_http_step_async(Arena *arena, Jsonv_Arena *jsonv_arena, ActiveJob *aj, long status_code) {
  /*#region*/
  // Complete processing and bind HTTP response variables back into workspace.
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
  if (aj->resp_buf.len > 0) {
    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
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

  char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
  jsonv_obj_set(jsonv_arena, aj->steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));

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
  if (aj->plugin_exec.child_resp_buf.len > 0) {
    Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
    if (temp_ctx) {
      if (jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)aj->plugin_exec.child_resp_buf.buf) ||
          jsonv_ctx_parse_yaml_data(temp_ctx, (const unsigned char *)aj->plugin_exec.child_resp_buf.buf)) {
        jsonv_ctx_get_value(temp_ctx, &body_val);
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

  char *step_id_cstr = allocate_jsonv_string(arena, step->id.data, step->id.length);
  jsonv_obj_set(jsonv_arena, aj->steps_state_obj.as.p, step_id_cstr, jsonv_val_obj(outcome_obj));

  plugin_cleanup(&aj->plugin_exec);
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t advance_active_job(Arena *arena, Jsonv_Arena *jsonv_arena, WorkflowAST *ast, Jsonv_Value *context_val, ActiveJob *aj, Transport *transport) {
  /*#region*/
  // Advance steps within an active job; executes non-blocking tasks inline.
  if (aj->job->execution_state == STATE_FAILED || aj->is_waiting_retry) {
    return ERR_SUCCESS;
  }

  Arena *effective_arena = aj->loop_arena ? aj->loop_arena : arena;

  while (aj->curr_step != NULL && aj->easy_handle == NULL && aj->plugin_exec.child_pid == 0) {
    StepNode *step = aj->curr_step;
    if (step->is_http) {
      int32_t status = transport->ops->start_request(transport, effective_arena, jsonv_arena, *context_val, step, &aj->resp_buf, &aj->easy_handle);
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

int32_t run_workflow_opt(Arena *arena, WorkflowAST *ast, Jsonv_Value *context_val, Transport *transport) {
  /*#region*/
  // Process the compiled graph concurrently using an event-driven scheduler.
  if (!arena || !ast || !context_val || !transport) return ERR_OOM;

  int32_t ret_val = ERR_SUCCESS;
  ActiveJob *active_jobs_head = NULL;

  ACNode *ac_root = ac_create_trie(arena, *context_val);
  set_global_ac_root(ac_root);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast->jsonv_ctx);

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
          }
        }
        job = job->next_sorted;
      }
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
        if (job->type == NODE_TASK || job->type == NODE_LOOP) {
          ActiveJob *aj = na_alloc(arena, sizeof(ActiveJob));
          if (!aj) {
            ret_val = ERR_OOM;
            goto cleanup;
          }
          memset(aj, 0, sizeof(ActiveJob));
          aj->job = job;

          if (job->type == NODE_TASK) {
            aj->steps_state_obj = jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL));
            char *k_steps = allocate_jsonv_string(arena, "steps", 5);
            jsonv_obj_set(jsonv_arena, context_val->as.p, k_steps, aj->steps_state_obj);
            aj->curr_step = job->spec.task.steps_head;
            aj->is_loop = false;
          } else {
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
          }

          job->execution_state = STATE_RUNNING;
          int32_t status = advance_active_job(arena, jsonv_arena, ast, context_val, aj, transport);
          if (status != ERR_SUCCESS) {
            ret_val = status;
            goto cleanup;
          }

          if (job->execution_state == STATE_RUNNING) {
            aj->next = active_jobs_head;
            active_jobs_head = aj;
          }
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
          int32_t status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_retry, transport);
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
              aj_http = aj_http->next;
              continue;
            }
            int32_t comp_status = complete_http_step_async(arena, jsonv_arena, aj_http, status_code);
            if (comp_status != ERR_SUCCESS) {
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
              aj_http = aj_http->next;
              continue;
            }
            aj_http->job->execution_state = STATE_FAILED;
          }

          transport->ops->cleanup_request(transport, aj_http->easy_handle);
          aj_http->easy_handle = NULL;
          aj_http->curr_step_retry_attempt = 0;
          aj_http->curr_step = aj_http->curr_step->next;

          int32_t adv_status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_http, transport);
          if (adv_status != ERR_SUCCESS) {
            ret_val = adv_status;
            goto cleanup;
          }
        }
      }
      aj_http = aj_http->next;
    }

    // Poll and read from active child processes (plugins)
    ActiveJob *aj_proc = active_jobs_head;
    while (aj_proc) {
      if (aj_proc->plugin_exec.child_pid > 0) {
        bool finished = false;
        long exit_code = 0;
        Arena *eff_arena = aj_proc->loop_arena ? aj_proc->loop_arena : arena;

        plugin_poll(&aj_proc->plugin_exec, eff_arena, jsonv_arena, context_val, aj_proc->curr_step, &finished, &exit_code);
        if (finished) {
          if (exit_code == -4) { // Timeout
            if (handle_step_failure(eff_arena, jsonv_arena, context_val, aj_proc, "Plugin timeout")) {
              aj_proc = aj_proc->next;
              continue;
            }
            complete_plugin_step_async(arena, jsonv_arena, aj_proc, -4);
            aj_proc->job->execution_state = STATE_FAILED;
          } else { // Exited
            if (exit_code != 0 && handle_step_failure(eff_arena, jsonv_arena, context_val, aj_proc, "Plugin exited with non-zero code")) {
              aj_proc = aj_proc->next;
              continue;
            }
            complete_plugin_step_async(arena, jsonv_arena, aj_proc, exit_code);
            aj_proc->curr_step_retry_attempt = 0;
            aj_proc->curr_step = aj_proc->curr_step->next;

            int32_t adv_status = advance_active_job(arena, jsonv_arena, ast, context_val, aj_proc, transport);
            if (adv_status != ERR_SUCCESS) {
              ret_val = adv_status;
              goto cleanup;
            }
          }
        }
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
