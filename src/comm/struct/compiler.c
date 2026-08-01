#include "compiler.h"
#include <string.h>
#include "evaluator.h"

void *na_alloc(Arena *arena, size_t size);

static bool is_ident_char(char c) {
  /*#region*/
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  /*#endregion*/
}

static bool expression_references_var(StringView expr, StringView var_name) {
  /*#region*/
  if (expr.length < var_name.length) return false;
  for (size_t i = 0; i <= expr.length - var_name.length; i++) {
    if (strncmp(expr.data + i, var_name.data, var_name.length) == 0) {
      // Check boundaries
      bool before_ok = (i == 0 || !is_ident_char(expr.data[i - 1]));
      bool after_ok = (i + var_name.length == expr.length || !is_ident_char(expr.data[i + var_name.length]));
      if (before_ok && after_ok) {
        return true;
      }
    }
  }
  return false;
  /*#endregion*/
}

static int32_t compile_variables(Arena *arena, Jsonv_Value vars_val, VariableAST **out_head) {
  /*#region*/
  if (vars_val.tag != JSONV_VAL_ARRAY) {
    return ERR_MISSING_VAR;
  }
  int len = jsonv_arr_length(vars_val.as.p);
  VariableAST *head = NULL;
  VariableAST *tail = NULL;
  for (int i = 0; i < len; i++) {
    Jsonv_Value var_item = jsonv_arr_val_at(vars_val.as.p, i);
    if (var_item.tag != JSONV_VAL_OBJ) return ERR_MISSING_VAR;

    Jsonv_Value v_name, v_expr, v_vis;
    if (!jsonv_obj_get(var_item.as.p, "name", &v_name) || v_name.tag != JSONV_VAL_STRING) {
      return ERR_MISSING_VAR;
    }
    if (!jsonv_obj_get(var_item.as.p, "expression", &v_expr) || v_expr.tag != JSONV_VAL_STRING) {
      return ERR_MISSING_VAR;
    }

    StringView name = { v_name.as.p, jsonv_val_str_len(v_name) };
    StringView expr = { v_expr.as.p, jsonv_val_str_len(v_expr) };

    // Validate naming rules (snake_case/camelCase): start with alpha/underscore, followed by alnum/underscore.
    if (name.length == 0) return ERR_MISSING_VAR;
    if (!((name.data[0] >= 'a' && name.data[0] <= 'z') ||
          (name.data[0] >= 'A' && name.data[0] <= 'Z') ||
          name.data[0] == '_')) {
      return ERR_MISSING_VAR;
    }
    for (size_t j = 1; j < name.length; j++) {
      char c = name.data[j];
      if (!((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_')) {
        return ERR_MISSING_VAR;
      }
    }

    VarVisibility visibility = VAR_PRIVATE;
    if (jsonv_obj_get(var_item.as.p, "visibility", &v_vis) && v_vis.tag == JSONV_VAL_STRING) {
      StringView vis_sv = { v_vis.as.p, jsonv_val_str_len(v_vis) };
      if (sv_equals_cstr(vis_sv, "public")) {
        visibility = VAR_PUBLIC;
      } else if (sv_equals_cstr(vis_sv, "private")) {
        visibility = VAR_PRIVATE;
      } else {
        return ERR_MISSING_VAR;
      }
    }

    VariableAST *var = na_alloc(arena, sizeof(VariableAST));
    if (!var) return ERR_OOM;
    memset(var, 0, sizeof(VariableAST));
    var->name = name;
    var->expression = expr;
    var->visibility = visibility;

    if (!head) {
      head = var;
    } else {
      tail->next = var;
    }
    tail = var;
  }

  // Topologically sort variables to check for cycle and define ordered list
  if (len > 0) {
    VariableAST **var_arr = na_alloc(arena, len * sizeof(VariableAST *));
    if (!var_arr) return ERR_OOM;
    VariableAST *curr = head;
    for (int i = 0; i < len; i++) {
      var_arr[i] = curr;
      curr = curr->next;
    }

    int *in_degrees = na_alloc(arena, len * sizeof(int));
    if (!in_degrees) return ERR_OOM;
    memset(in_degrees, 0, len * sizeof(int));

    for (int i = 0; i < len; i++) {
      for (int j = 0; j < len; j++) {
        if (i == j) continue;
        if (expression_references_var(var_arr[j]->expression, var_arr[i]->name)) {
          in_degrees[j]++;
        }
      }
    }

    VariableAST **queue = na_alloc(arena, len * sizeof(VariableAST *));
    if (!queue) return ERR_OOM;
    int q_head = 0;
    int q_tail = 0;
    for (int i = 0; i < len; i++) {
      if (in_degrees[i] == 0) {
        queue[q_tail++] = var_arr[i];
      }
    }

    VariableAST *sorted_head = NULL;
    VariableAST *sorted_tail = NULL;
    int sorted_count = 0;

    while (q_head < q_tail) {
      VariableAST *u = queue[q_head++];
      u->next = NULL;
      if (!sorted_head) {
        sorted_head = u;
        sorted_tail = u;
      } else {
        sorted_tail->next = u;
        sorted_tail = u;
      }
      sorted_count++;

      for (int i = 0; i < len; i++) {
        VariableAST *v = var_arr[i];
        if (v != u && expression_references_var(v->expression, u->name)) {
          int v_idx = -1;
          for (int k = 0; k < len; k++) {
            if (var_arr[k] == v) {
              v_idx = k;
              break;
            }
          }
          if (v_idx != -1) {
            in_degrees[v_idx]--;
            if (in_degrees[v_idx] == 0) {
              queue[q_tail++] = v;
            }
          }
        }
      }
    }

    if (sorted_count < len) {
      return ERR_CYCLIC_DEP;
    }
    head = sorted_head;
  }

  *out_head = head;
  return ERR_SUCCESS;
  /*#endregion*/
}


static int32_t compile_step_outputs(Arena *arena, Jsonv_Value outputs_val, VariableAST **out_head) {
  /*#region*/
  if (outputs_val.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }
  Jsonv_Obj *obj = outputs_val.as.p;
  int len = jsonv_obj_length(obj);
  VariableAST *head = NULL;
  VariableAST *tail = NULL;
  for (int i = 0; i < len; i++) {
    const char *key = jsonv_obj_key_at(obj, i);
    Jsonv_Value expr_val = jsonv_obj_val_at(obj, i);
    if (expr_val.tag != JSONV_VAL_STRING) {
      return ERR_MISSING_VAR;
    }

    StringView name = { key, strlen(key) };
    StringView expr = { expr_val.as.p, jsonv_val_str_len(expr_val) };

    if (name.length == 0) return ERR_MISSING_VAR;
    if (!((name.data[0] >= 'a' && name.data[0] <= 'z') ||
          (name.data[0] >= 'A' && name.data[0] <= 'Z') ||
          name.data[0] == '_')) {
      return ERR_MISSING_VAR;
    }
    for (size_t j = 1; j < name.length; j++) {
      char c = name.data[j];
      if (!((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_')) {
        return ERR_MISSING_VAR;
      }
    }

    VariableAST *var = na_alloc(arena, sizeof(VariableAST));
    if (!var) return ERR_OOM;
    memset(var, 0, sizeof(VariableAST));
    var->name = name;
    var->expression = expr;
    var->visibility = VAR_PUBLIC;

    if (!head) {
      head = var;
    } else {
      tail->next = var;
    }
    tail = var;
  }

  *out_head = head;
  return ERR_SUCCESS;
  /*#endregion*/
}


static bool is_operation_resource(StringView sv) {
  /*#region*/
  char buf[256];
  if (sv.length >= sizeof(buf)) {
    return true;
  }
  memcpy(buf, sv.data, sv.length);
  buf[sv.length] = '\0';
  for (size_t i = 0; i < sv.length; i++) {
    if (buf[i] >= 'A' && buf[i] <= 'Z') {
      buf[i] = buf[i] - 'A' + 'a';
    }
  }
  if (strstr(buf, "query") || strstr(buf, "get") || strstr(buf, "read") ||
      strstr(buf, "fetch") || strstr(buf, "select") || strstr(buf, "list")) {
    return false;
  }
  return true;
  /*#endregion*/
}

static int32_t compile_step(Arena *arena, Jsonv_Value step_val, StepNode **out_step) {
  /*#region*/
  if (step_val.tag != JSONV_VAL_OBJ)
    return ERR_MISSING_VAR;

  StepNode *step = na_alloc(arena, sizeof(StepNode));
  if (!step)
    return ERR_OOM;
  memset(step, 0, sizeof(StepNode));

  Jsonv_Value step_id_val;
  if (jsonv_obj_get(step_val.as.p, "id", &step_id_val) && step_id_val.tag == JSONV_VAL_STRING) {
    step->id.data = step_id_val.as.p;
    step->id.length = jsonv_val_str_len(step_id_val);
  }

  Jsonv_Value step_http;
  Jsonv_Value step_uses;
  Jsonv_Value step_provider;
  if (jsonv_obj_get(step_val.as.p, "http", &step_http) && step_http.tag == JSONV_VAL_OBJ) {
    step->is_http = true;
    Jsonv_Value v_method;
    if (!jsonv_obj_get(step_http.as.p, "method", &v_method) || v_method.tag != JSONV_VAL_STRING) {
      return ERR_MISSING_VAR;
    }
    step->http.method.data = v_method.as.p;
    step->http.method.length = jsonv_val_str_len(v_method);

    Jsonv_Value v_url;
    if (!jsonv_obj_get(step_http.as.p, "url", &v_url) || v_url.tag != JSONV_VAL_STRING) {
      return ERR_MISSING_VAR;
    }
    step->http.url.data = v_url.as.p;
    step->http.url.length = jsonv_val_str_len(v_url);

    jsonv_obj_get(step_http.as.p, "headers", &step->http.headers);
    jsonv_obj_get(step_http.as.p, "body", &step->http.body);

    Jsonv_Value v_timeout;
    if (jsonv_obj_get(step_http.as.p, "timeout", &v_timeout) && v_timeout.tag == JSONV_VAL_STRING) {
      step->timeout.data = v_timeout.as.p;
      step->timeout.length = jsonv_val_str_len(v_timeout);
    }
    Jsonv_Value v_mtls;
    if (jsonv_obj_get(step_http.as.p, "mtls_profile", &v_mtls) && v_mtls.tag == JSONV_VAL_STRING) {
      step->http.mtls_profile.data = v_mtls.as.p;
      step->http.mtls_profile.length = jsonv_val_str_len(v_mtls);
    }
    Jsonv_Value v_stream;
    step->http.stream = false;
    if (jsonv_obj_get(step_http.as.p, "stream", &v_stream) && v_stream.tag == JSONV_VAL_BOOLEAN) {
      step->http.stream = v_stream.as.boolean;
    }
    Jsonv_Value v_chunk_size;
    step->http.chunk_size = 0;
    if (jsonv_obj_get(step_http.as.p, "chunk_size", &v_chunk_size)) {
      if (v_chunk_size.tag == JSONV_VAL_INT) {
        step->http.chunk_size = (int)v_chunk_size.as.i;
      } else if (v_chunk_size.tag == JSONV_VAL_DOUBLE) {
        step->http.chunk_size = (int)v_chunk_size.as.d;
      }
    }
    if (sv_equals_cstr(step->http.method, "GET") || sv_equals_cstr(step->http.method, "HEAD")) {
      step->is_resource = false;
    } else {
      step->is_resource = true;
    }
  } else if (jsonv_obj_get(step_val.as.p, "uses", &step_uses) && step_uses.tag == JSONV_VAL_STRING) {
    step->is_http = false;
    step->plugin.uses.data = step_uses.as.p;
    step->plugin.uses.length = jsonv_val_str_len(step_uses);
    jsonv_obj_get(step_val.as.p, "with", &step->plugin.with_args);
    Jsonv_Value v_sandboxed;
    step->plugin.sandboxed = false;
    if (jsonv_obj_get(step_val.as.p, "sandboxed", &v_sandboxed) && v_sandboxed.tag == JSONV_VAL_BOOLEAN) {
      step->plugin.sandboxed = v_sandboxed.as.boolean;
    }
    Jsonv_Value v_timeout;
    if (jsonv_obj_get(step_val.as.p, "timeout", &v_timeout) && v_timeout.tag == JSONV_VAL_STRING) {
      step->timeout.data = v_timeout.as.p;
      step->timeout.length = jsonv_val_str_len(v_timeout);
    }
    step->is_resource = is_operation_resource(step->plugin.uses);
  } else if (jsonv_obj_get(step_val.as.p, "provider", &step_provider) && step_provider.tag == JSONV_VAL_STRING) {
    step->is_http = false;
    step->is_provider = true;
    step->prov.provider.data = step_provider.as.p;
    step->prov.provider.length = jsonv_val_str_len(step_provider);
    jsonv_obj_get(step_val.as.p, "args", &step->prov.args);
    Jsonv_Value v_timeout;
    if (jsonv_obj_get(step_val.as.p, "timeout", &v_timeout) && v_timeout.tag == JSONV_VAL_STRING) {
      step->timeout.data = v_timeout.as.p;
      step->timeout.length = jsonv_val_str_len(v_timeout);
    }
    step->is_resource = is_operation_resource(step->prov.provider);
  } else {
    return ERR_MISSING_VAR;
  }

  // Parse step-level retries (if present)
  step->retry_attempts = 0;
  Jsonv_Value v_ret_att;
  if (jsonv_obj_get(step_val.as.p, "retry_attempts", &v_ret_att)) {
    if (v_ret_att.tag == JSONV_VAL_INT) {
      step->retry_attempts = (int)v_ret_att.as.i;
    } else if (v_ret_att.tag == JSONV_VAL_DOUBLE) {
      step->retry_attempts = (int)v_ret_att.as.d;
    }
  }
  Jsonv_Value v_ret_bk;
  if (jsonv_obj_get(step_val.as.p, "retry_backoff", &v_ret_bk) && v_ret_bk.tag == JSONV_VAL_STRING) {
    step->retry_backoff.data = v_ret_bk.as.p;
    step->retry_backoff.length = jsonv_val_str_len(v_ret_bk);
  }
  Jsonv_Value v_ret_dl;
  if (jsonv_obj_get(step_val.as.p, "retry_delay", &v_ret_dl) && v_ret_dl.tag == JSONV_VAL_STRING) {
    step->retry_delay.data = v_ret_dl.as.p;
    step->retry_delay.length = jsonv_val_str_len(v_ret_dl);
  }
  Jsonv_Value v_vars;
  if (jsonv_obj_get(step_val.as.p, "variables", &v_vars) && v_vars.tag == JSONV_VAL_ARRAY) {
    int32_t status = compile_variables(arena, v_vars, &step->variables_head);
    if (status != ERR_SUCCESS) {
      return status;
    }
  }
  Jsonv_Value v_outputs;
  if (jsonv_obj_get(step_val.as.p, "outputs", &v_outputs) && v_outputs.tag == JSONV_VAL_OBJ) {
    int32_t status = compile_step_outputs(arena, v_outputs, &step->outputs_head);
    if (status != ERR_SUCCESS) {
      return status;
    }
  }

  Jsonv_Value v_on_error;
  if (jsonv_obj_get(step_val.as.p, "on_error", &v_on_error) && v_on_error.tag == JSONV_VAL_OBJ) {
    Jsonv_Value v_fallback;
    if (jsonv_obj_get(v_on_error.as.p, "fallback", &v_fallback)) {
      step->has_on_error = true;
      step->fallback = v_fallback;
    }
  }

  *out_step = step;
  return ERR_SUCCESS;
  /*#endregion*/
}

static bool job_targets_id(JobNode *u, StringView id) {
  /*#region*/
  // Determine if a control-flow node u targets another node id.
  if (u->type == NODE_IF) {
    for (size_t i = 0; i < u->spec.binary_if.then_count; i++) {
      if (sv_compare(u->spec.binary_if.then_branch[i], id) == 0) return true;
    }
    for (size_t i = 0; i < u->spec.binary_if.else_count; i++) {
      if (sv_compare(u->spec.binary_if.else_branch[i], id) == 0) return true;
    }
  } else if (u->type == NODE_SWITCH) {
    SwitchCase *sc = u->spec.multi_switch.cases;
    while (sc) {
      for (size_t i = 0; i < sc->then_count; i++) {
        if (sv_compare(sc->then_branch[i], id) == 0) return true;
      }
      sc = sc->next;
    }
    for (size_t i = 0; i < u->spec.multi_switch.default_count; i++) {
      if (sv_compare(u->spec.multi_switch.default_branch[i], id) == 0) return true;
    }
  } else if (u->type == NODE_FORK) {
    for (size_t i = 0; i < u->spec.fork_node.branch_count; i++) {
      if (sv_compare(u->spec.fork_node.branches[i], id) == 0) return true;
    }
  }
  return false;
  /*#endregion*/
}

static int32_t compile_task_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_steps;
  if (!jsonv_obj_get(job_spec_val.as.p, "steps", &v_steps) || v_steps.tag != JSONV_VAL_ARRAY) {
    return ERR_MISSING_VAR;
  }
  int steps_len = jsonv_arr_length(v_steps.as.p);
  StepNode *prev_step = NULL;
  for (int s = 0; s < steps_len; s++) {
    Jsonv_Value step_val = jsonv_arr_val_at(v_steps.as.p, s);
    StepNode *step = NULL;
    int32_t step_status = compile_step(arena, step_val, &step);
    if (step_status != ERR_SUCCESS) {
      return step_status;
    }

    if (!job->spec.task.steps_head) {
      job->spec.task.steps_head = step;
    } else {
      prev_step->next = step;
    }
    prev_step = step;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_if_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_cond;
  if (!jsonv_obj_get(job_spec_val.as.p, "condition", &v_cond) || v_cond.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  job->spec.binary_if.condition.data = v_cond.as.p;
  job->spec.binary_if.condition.length = jsonv_val_str_len(v_cond);

  Jsonv_Value v_then;
  if (jsonv_obj_get(job_spec_val.as.p, "then", &v_then) && v_then.tag == JSONV_VAL_ARRAY) {
    int count = jsonv_arr_length(v_then.as.p);
    job->spec.binary_if.then_count = count;
    job->spec.binary_if.then_branch = na_alloc(arena, count * sizeof(StringView));
    if (!job->spec.binary_if.then_branch && count > 0)
      return ERR_OOM;
    for (int c = 0; c < count; c++) {
      Jsonv_Value val = jsonv_arr_val_at(v_then.as.p, c);
      if (val.tag != JSONV_VAL_STRING)
        return ERR_MISSING_VAR;
      job->spec.binary_if.then_branch[c].data = val.as.p;
      job->spec.binary_if.then_branch[c].length = jsonv_val_str_len(val);
    }
  }
  Jsonv_Value v_else;
  if (jsonv_obj_get(job_spec_val.as.p, "else", &v_else) && v_else.tag == JSONV_VAL_ARRAY) {
    int count = jsonv_arr_length(v_else.as.p);
    job->spec.binary_if.else_count = count;
    job->spec.binary_if.else_branch = na_alloc(arena, count * sizeof(StringView));
    if (!job->spec.binary_if.else_branch && count > 0)
      return ERR_OOM;
    for (int c = 0; c < count; c++) {
      Jsonv_Value val = jsonv_arr_val_at(v_else.as.p, c);
      if (val.tag != JSONV_VAL_STRING)
        return ERR_MISSING_VAR;
      job->spec.binary_if.else_branch[c].data = val.as.p;
      job->spec.binary_if.else_branch[c].length = jsonv_val_str_len(val);
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_switch_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_cases;
  if (jsonv_obj_get(job_spec_val.as.p, "cases", &v_cases) && v_cases.tag == JSONV_VAL_ARRAY) {
    int cases_len = jsonv_arr_length(v_cases.as.p);
    job->spec.multi_switch.case_count = cases_len;
    SwitchCase *prev_case = NULL;
    for (int c = 0; c < cases_len; c++) {
      Jsonv_Value case_val = jsonv_arr_val_at(v_cases.as.p, c);
      if (case_val.tag != JSONV_VAL_OBJ)
        return ERR_MISSING_VAR;

      SwitchCase *sc = na_alloc(arena, sizeof(SwitchCase));
      if (!sc)
        return ERR_OOM;
      memset(sc, 0, sizeof(SwitchCase));

      Jsonv_Value v_cond;
      if (!jsonv_obj_get(case_val.as.p, "condition", &v_cond) || v_cond.tag != JSONV_VAL_STRING) {
        return ERR_MISSING_VAR;
      }
      sc->condition.data = v_cond.as.p;
      sc->condition.length = jsonv_val_str_len(v_cond);

      Jsonv_Value v_then;
      if (jsonv_obj_get(case_val.as.p, "then", &v_then) && v_then.tag == JSONV_VAL_ARRAY) {
        int count = jsonv_arr_length(v_then.as.p);
        sc->then_count = count;
        sc->then_branch = na_alloc(arena, count * sizeof(StringView));
        if (!sc->then_branch && count > 0)
          return ERR_OOM;
        for (int t = 0; t < count; t++) {
          Jsonv_Value val = jsonv_arr_val_at(v_then.as.p, t);
          if (val.tag != JSONV_VAL_STRING)
            return ERR_MISSING_VAR;
          sc->then_branch[t].data = val.as.p;
          sc->then_branch[t].length = jsonv_val_str_len(val);
        }
      }

      if (!job->spec.multi_switch.cases) {
        job->spec.multi_switch.cases = sc;
      } else {
        prev_case->next = sc;
      }
      prev_case = sc;
    }
  }
  Jsonv_Value v_def;
  if (jsonv_obj_get(job_spec_val.as.p, "default", &v_def) && v_def.tag == JSONV_VAL_ARRAY) {
    int count = jsonv_arr_length(v_def.as.p);
    job->spec.multi_switch.default_count = count;
    job->spec.multi_switch.default_branch = na_alloc(arena, count * sizeof(StringView));
    if (!job->spec.multi_switch.default_branch && count > 0)
      return ERR_OOM;
    for (int d = 0; d < count; d++) {
      Jsonv_Value val = jsonv_arr_val_at(v_def.as.p, d);
      if (val.tag != JSONV_VAL_STRING)
        return ERR_MISSING_VAR;
      job->spec.multi_switch.default_branch[d].data = val.as.p;
      job->spec.multi_switch.default_branch[d].length = jsonv_val_str_len(val);
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_fork_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_branches;
  if (!jsonv_obj_get(job_spec_val.as.p, "branches", &v_branches) || v_branches.tag != JSONV_VAL_ARRAY) {
    return ERR_MISSING_VAR;
  }
  int count = jsonv_arr_length(v_branches.as.p);
  job->spec.fork_node.branch_count = count;
  job->spec.fork_node.branches = na_alloc(arena, count * sizeof(StringView));
  if (!job->spec.fork_node.branches && count > 0)
    return ERR_OOM;
  for (int b = 0; b < count; b++) {
    Jsonv_Value val = jsonv_arr_val_at(v_branches.as.p, b);
    if (val.tag != JSONV_VAL_STRING)
      return ERR_MISSING_VAR;
    job->spec.fork_node.branches[b].data = val.as.p;
    job->spec.fork_node.branches[b].length = jsonv_val_str_len(val);
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_join_spec(JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_strat;
  if (jsonv_obj_get(job_spec_val.as.p, "strategy", &v_strat) && v_strat.tag == JSONV_VAL_STRING) {
    job->spec.join_node.strategy.data = v_strat.as.p;
    job->spec.join_node.strategy.length = jsonv_val_str_len(v_strat);
  } else {
    job->spec.join_node.strategy.data = "all";
    job->spec.join_node.strategy.length = 3;
  }
  Jsonv_Value v_req;
  if (jsonv_obj_get(job_spec_val.as.p, "n_required", &v_req)) {
    if (v_req.tag == JSONV_VAL_INT) {
      job->spec.join_node.n_required = v_req.as.i;
    } else if (v_req.tag == JSONV_VAL_DOUBLE) {
      job->spec.join_node.n_required = (size_t)v_req.as.d;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_loop_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value v_loop_type;
  if (jsonv_obj_get(job_spec_val.as.p, "loop_type", &v_loop_type) && v_loop_type.tag == JSONV_VAL_STRING) {
    job->spec.loop_node.loop_type.data = v_loop_type.as.p;
    job->spec.loop_node.loop_type.length = jsonv_val_str_len(v_loop_type);
  }
  Jsonv_Value v_cond;
  if (jsonv_obj_get(job_spec_val.as.p, "condition", &v_cond) && v_cond.tag == JSONV_VAL_STRING) {
    job->spec.loop_node.condition.data = v_cond.as.p;
    job->spec.loop_node.condition.length = jsonv_val_str_len(v_cond);
  }
  Jsonv_Value v_items;
  if (jsonv_obj_get(job_spec_val.as.p, "items", &v_items) && v_items.tag == JSONV_VAL_STRING) {
    job->spec.loop_node.items.data = v_items.as.p;
    job->spec.loop_node.items.length = jsonv_val_str_len(v_items);
  }
  Jsonv_Value v_max_iter;
  if (jsonv_obj_get(job_spec_val.as.p, "max_iterations", &v_max_iter)) {
    if (v_max_iter.tag == JSONV_VAL_INT) {
      job->spec.loop_node.max_iterations = v_max_iter.as.i;
    } else if (v_max_iter.tag == JSONV_VAL_DOUBLE) {
      job->spec.loop_node.max_iterations = (size_t)v_max_iter.as.d;
    }
  }
  Jsonv_Value v_source;
  if (jsonv_obj_get(job_spec_val.as.p, "source", &v_source) && v_source.tag == JSONV_VAL_STRING) {
    job->spec.loop_node.source.data = v_source.as.p;
    job->spec.loop_node.source.length = jsonv_val_str_len(v_source);
  }
  Jsonv_Value v_chunk_limit;
  job->spec.loop_node.chunk_record_limit = 1000; // default chunk record limit
  if (jsonv_obj_get(job_spec_val.as.p, "chunk_record_limit", &v_chunk_limit)) {
    if (v_chunk_limit.tag == JSONV_VAL_INT) {
      job->spec.loop_node.chunk_record_limit = (size_t)v_chunk_limit.as.i;
    } else if (v_chunk_limit.tag == JSONV_VAL_DOUBLE) {
      job->spec.loop_node.chunk_record_limit = (size_t)v_chunk_limit.as.d;
    }
  }

  Jsonv_Value v_steps;
  if (jsonv_obj_get(job_spec_val.as.p, "steps", &v_steps) && v_steps.tag == JSONV_VAL_ARRAY) {
    int steps_len = jsonv_arr_length(v_steps.as.p);
    StepNode *prev_step = NULL;
    for (int s = 0; s < steps_len; s++) {
      Jsonv_Value step_val = jsonv_arr_val_at(v_steps.as.p, s);
      StepNode *step = NULL;
      int32_t step_status = compile_step(arena, step_val, &step);
      if (step_status != ERR_SUCCESS) {
        return step_status;
      }

      if (!job->spec.loop_node.steps_head) {
        job->spec.loop_node.steps_head = step;
      } else {
        prev_step->next = step;
      }
      prev_step = step;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_wait_signal_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value target_val = job_spec_val;
  Jsonv_Value v_spec;
  if (jsonv_obj_get(job_spec_val.as.p, "spec", &v_spec) && v_spec.tag == JSONV_VAL_OBJ) {
    target_val = v_spec;
  }

  Jsonv_Value v_corr;
  if (!jsonv_obj_get(target_val.as.p, "correlation_id", &v_corr) || v_corr.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  job->spec.wait_signal.correlation_id.data = v_corr.as.p;
  job->spec.wait_signal.correlation_id.length = jsonv_val_str_len(v_corr);

  Jsonv_Value v_timeout;
  if (jsonv_obj_get(target_val.as.p, "timeout", &v_timeout) && v_timeout.tag == JSONV_VAL_STRING) {
    job->spec.wait_signal.timeout.data = v_timeout.as.p;
    job->spec.wait_signal.timeout.length = jsonv_val_str_len(v_timeout);
  }

  Jsonv_Value v_steps;
  if (jsonv_obj_get(target_val.as.p, "steps", &v_steps) && v_steps.tag == JSONV_VAL_ARRAY) {
    int steps_len = jsonv_arr_length(v_steps.as.p);
    StepNode *prev_step = NULL;
    for (int s = 0; s < steps_len; s++) {
      Jsonv_Value step_val = jsonv_arr_val_at(v_steps.as.p, s);
      StepNode *step = NULL;
      int32_t step_status = compile_step(arena, step_val, &step);
      if (step_status != ERR_SUCCESS) {
        return step_status;
      }

      if (!job->spec.wait_signal.steps_head) {
        job->spec.wait_signal.steps_head = step;
      } else {
        prev_step->next = step;
      }
      prev_step = step;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_wait_timer_spec(JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  Jsonv_Value target_val = job_spec_val;
  Jsonv_Value v_spec;
  if (jsonv_obj_get(job_spec_val.as.p, "spec", &v_spec) && v_spec.tag == JSONV_VAL_OBJ) {
    target_val = v_spec;
  }

  Jsonv_Value v_dur;
  if (!jsonv_obj_get(target_val.as.p, "duration", &v_dur) || v_dur.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  job->spec.wait_timer.duration.data = v_dur.as.p;
  job->spec.wait_timer.duration.length = jsonv_val_str_len(v_dur);
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_transform_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  (void)arena;
  Jsonv_Value v_spec;
  if (!jsonv_obj_get(job_spec_val.as.p, "spec", &v_spec) || v_spec.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }
  Jsonv_Value v_expr;
  if (!jsonv_obj_get(v_spec.as.p, "expression", &v_expr) || v_expr.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  job->spec.transform.expression.data = v_expr.as.p;
  job->spec.transform.expression.length = jsonv_val_str_len(v_expr);
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_export_spec(Arena *arena, JobNode *job, Jsonv_Value job_spec_val) {
  /*#region*/
  (void)arena;
  Jsonv_Value v_spec;
  if (!jsonv_obj_get(job_spec_val.as.p, "spec", &v_spec) || v_spec.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }
  Jsonv_Value v_file;
  if (!jsonv_obj_get(v_spec.as.p, "file", &v_file) || v_file.tag != JSONV_VAL_STRING) {
    return ERR_MISSING_VAR;
  }
  Jsonv_Value v_data;
  if (!jsonv_obj_get(v_spec.as.p, "data", &v_data)) {
    return ERR_MISSING_VAR;
  }
  job->spec.export_node.file_path.data = v_file.as.p;
  job->spec.export_node.file_path.length = jsonv_val_str_len(v_file);
  job->spec.export_node.data_val = v_data;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t inject_implicit_dependencies(Arena *arena, JobNode **job_nodes, int job_count) {
  /*#region*/
  for (int i = 0; i < job_count; i++) {
    JobNode *v = job_nodes[i];
    size_t implicit_count = 0;
    for (int j = 0; j < job_count; j++) {
      if (i != j && job_targets_id(job_nodes[j], v->id)) {
        implicit_count++;
      }
    }
    if (implicit_count > 0) {
      size_t total_count = v->dependency_count + implicit_count;
      StringView *new_ids = na_alloc(arena, total_count * sizeof(StringView));
      JobNode **new_nodes = na_alloc(arena, total_count * sizeof(JobNode *));
      uint8_t *new_conditions = na_alloc(arena, total_count * sizeof(uint8_t));
      if (!new_ids || !new_nodes || !new_conditions) return ERR_OOM;
      if (v->dependency_count > 0) {
        memcpy(new_ids, v->depends_on_ids, v->dependency_count * sizeof(StringView));
        memcpy(new_nodes, v->depends_on_nodes, v->dependency_count * sizeof(JobNode *));
        if (v->depends_on_conditions) {
          memcpy(new_conditions, v->depends_on_conditions, v->dependency_count * sizeof(uint8_t));
        } else {
          memset(new_conditions, DEP_COND_SUCCESS, v->dependency_count * sizeof(uint8_t));
        }
      }
      size_t idx = v->dependency_count;
      for (int j = 0; j < job_count; j++) {
        if (i != j && job_targets_id(job_nodes[j], v->id)) {
          new_ids[idx] = job_nodes[j]->id;
          new_nodes[idx] = NULL;
          new_conditions[idx] = DEP_COND_SUCCESS;
          idx++;
        }
      }
      v->depends_on_ids = new_ids;
      v->depends_on_nodes = new_nodes;
      v->depends_on_conditions = new_conditions;
      v->dependency_count = total_count;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t resolve_dependencies(JobNode **job_nodes, int job_count) {
  /*#region*/
  for (int i = 0; i < job_count; i++) {
    JobNode *job = job_nodes[i];
    for (size_t d = 0; d < job->dependency_count; d++) {
      JobNode *dep_node = NULL;
      for (int k = 0; k < job_count; k++) {
        if (sv_compare(job->depends_on_ids[d], job_nodes[k]->id) == 0) {
          dep_node = job_nodes[k];
          break;
        }
      }
      if (!dep_node) {
        return ERR_MISSING_VAR; // Dependency does not exist
      }
      job->depends_on_nodes[d] = dep_node;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t topological_sort(Arena *arena, JobNode **job_nodes, int job_count, JobNode **out_sorted_head, size_t *out_sorted_count) {
  /*#region*/
  int *in_degrees = na_alloc(arena, job_count * sizeof(int));
  if (!in_degrees)
    return ERR_OOM;
  for (int i = 0; i < job_count; i++) {
    in_degrees[i] = (int)job_nodes[i]->dependency_count;
  }

  JobNode **queue = na_alloc(arena, job_count * sizeof(JobNode *));
  if (!queue)
    return ERR_OOM;
  int q_head = 0;
  int q_tail = 0;

  for (int i = 0; i < job_count; i++) {
    if (in_degrees[i] == 0) {
      queue[q_tail++] = job_nodes[i];
    }
  }

  JobNode *sorted_head = NULL;
  JobNode *sorted_tail = NULL;
  int sorted_count = 0;

  while (q_head < q_tail) {
    JobNode *u = queue[q_head++];

    u->next_sorted = NULL;
    if (!sorted_head) {
      sorted_head = u;
      sorted_tail = u;
    } else {
      sorted_tail->next_sorted = u;
      sorted_tail = u;
    }
    sorted_count++;

    // Decrement in-degree of all nodes depending on u
    for (int i = 0; i < job_count; i++) {
      JobNode *v = job_nodes[i];
      for (size_t d = 0; d < v->dependency_count; d++) {
        if (v->depends_on_nodes[d] == u) {
          in_degrees[i]--;
          if (in_degrees[i] == 0) {
            queue[q_tail++] = v;
          }
          break;
        }
      }
    }
  }

  if (sorted_count < job_count) {
    return ERR_CYCLIC_DEP;
  }

  *out_sorted_head = sorted_head;
  *out_sorted_count = (size_t)sorted_count;
  return ERR_SUCCESS;
  /*#endregion*/
}

static bool has_unjoined_exit_path(JobNode *current, JobNode **job_nodes, int job_count, bool *visited, int current_idx) {
  /*#region*/
  if (visited[current_idx]) {
    return false;
  }
  visited[current_idx] = true;

  if (current->type == NODE_JOIN) {
    return false;
  }

  if (current->is_end || current->return_expr.length > 0) {
    return true;
  }

  for (int i = 0; i < job_count; i++) {
    JobNode *v = job_nodes[i];
    for (size_t d = 0; d < v->dependency_count; d++) {
      if (v->depends_on_nodes[d] == current) {
        if (has_unjoined_exit_path(v, job_nodes, job_count, visited, i)) {
          return true;
        }
      }
    }
  }

  return false;
  /*#endregion*/
}

static int32_t validate_boundaries(Arena *arena, JobNode **job_nodes, int job_count) {
  /*#region*/
  int start_jobs_count = 0;
  int root_jobs_count = 0;
  JobNode *inferred_start_job = NULL;

  for (int i = 0; i < job_count; i++) {
    if (job_nodes[i]->is_start) {
      start_jobs_count++;
    }
    if (job_nodes[i]->dependency_count == 0) {
      root_jobs_count++;
      inferred_start_job = job_nodes[i];
    }
  }

  if (start_jobs_count > 1) {
    return ERR_INVALID_BOUNDARY;
  }

  if (start_jobs_count == 0) {
    if (root_jobs_count == 1) {
      inferred_start_job->is_start = true;
    } else if (root_jobs_count == 0) {
      return ERR_CYCLIC_DEP;
    } else {
      return ERR_INVALID_BOUNDARY;
    }
  }

  for (int i = 0; i < job_count; i++) {
    JobNode *job = job_nodes[i];
    if (job->type == NODE_FORK) {
      for (size_t b = 0; b < job->spec.fork_node.branch_count; b++) {
        StringView branch_id = job->spec.fork_node.branches[b];
        JobNode *branch_job = NULL;
        int branch_idx = -1;
        for (int k = 0; k < job_count; k++) {
          if (sv_compare(branch_id, job_nodes[k]->id) == 0) {
            branch_job = job_nodes[k];
            branch_idx = k;
            break;
          }
        }
        if (!branch_job) {
          return ERR_MISSING_VAR;
        }

        bool *visited = na_alloc(arena, job_count * sizeof(bool));
        if (!visited) return ERR_OOM;
        memset(visited, 0, job_count * sizeof(bool));

        if (has_unjoined_exit_path(branch_job, job_nodes, job_count, visited, branch_idx)) {
          return ERR_INVALID_BOUNDARY;
        }
      }
    }
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t compile_providers(Arena *arena, Jsonv_Value providers_val, ProviderConfigAST **out_head) {
  /*#region*/
  if (providers_val.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }
  Jsonv_Obj *obj = providers_val.as.p;
  int len = jsonv_obj_length(obj);
  ProviderConfigAST *head = NULL;
  ProviderConfigAST *tail = NULL;
  for (int i = 0; i < len; i++) {
    const char *key = jsonv_obj_key_at(obj, i);
    Jsonv_Value config_val = jsonv_obj_val_at(obj, i);
    if (config_val.tag != JSONV_VAL_OBJ) {
      return ERR_MISSING_VAR;
    }

    StringView name = { key, strlen(key) };

    ProviderConfigAST *pc = na_alloc(arena, sizeof(ProviderConfigAST));
    if (!pc) return ERR_OOM;
    memset(pc, 0, sizeof(ProviderConfigAST));
    pc->name = name;
    pc->config_val = config_val;

    if (!head) {
      head = pc;
    } else {
      tail->next = pc;
    }
    tail = pc;
  }

  *out_head = head;
  return ERR_SUCCESS;
  /*#endregion*/
}


int32_t compile_workflow(Arena *arena, WorkflowAST *ast) {
  /*#region*/
  if (!arena || !ast)
    return ERR_OOM;

  Jsonv_Value root_val = ast->root_val;

  // Extract workflow root variables
  Jsonv_Value root_vars;
  if (jsonv_obj_get(root_val.as.p, "variables", &root_vars) && root_vars.tag == JSONV_VAL_ARRAY) {
    int32_t status = compile_variables(arena, root_vars, &ast->variables_head);
    if (status != ERR_SUCCESS) {
      return status;
    }
  }

  // Extract workflow root providers
  Jsonv_Value root_providers;
  if (jsonv_obj_get(root_val.as.p, "providers", &root_providers) && root_providers.tag == JSONV_VAL_OBJ) {
    int32_t status = compile_providers(arena, root_providers, &ast->providers_head);
    if (status != ERR_SUCCESS) {
      return status;
    }
  }

  Jsonv_Value jobs_val;
  if (!jsonv_obj_get(root_val.as.p, "jobs", &jobs_val) || jobs_val.tag != JSONV_VAL_OBJ) {
    return ERR_MISSING_VAR;
  }

  int job_count = jsonv_obj_length(jobs_val.as.p);
  if (job_count == 0) {
    ast->jobs_head = NULL;
    ast->job_count = 0;
    return ERR_SUCCESS;
  }

  JobNode **job_nodes = na_alloc(arena, job_count * sizeof(JobNode *));
  if (!job_nodes)
    return ERR_OOM;

  // 1. Parse each job spec into a JobNode
  for (int i = 0; i < job_count; i++) {
    const char *job_id_cstr = jsonv_obj_key_at(jobs_val.as.p, i);
    Jsonv_Value job_spec_val = jsonv_obj_val_at(jobs_val.as.p, i);
    if (job_spec_val.tag != JSONV_VAL_OBJ) {
      return ERR_MISSING_VAR;
    }

    JobNode *job = na_alloc(arena, sizeof(JobNode));
    if (!job)
      return ERR_OOM;
    memset(job, 0, sizeof(JobNode));

    job->id.data = job_id_cstr;
    job->id.length = strlen(job_id_cstr);
    job->execution_state = 0; // PENDING

    // Extract variables at the job level
    Jsonv_Value v_job_vars;
    if (jsonv_obj_get(job_spec_val.as.p, "variables", &v_job_vars) && v_job_vars.tag == JSONV_VAL_ARRAY) {
      int32_t status = compile_variables(arena, v_job_vars, &job->variables_head);
      if (status != ERR_SUCCESS) {
        return status;
      }
    }

    // Extract boundary properties
    Jsonv_Value v_start;
    if (jsonv_obj_get(job_spec_val.as.p, "start", &v_start)) {
      if (v_start.tag == JSONV_VAL_BOOLEAN) {
        job->is_start = v_start.as.boolean;
      } else {
        return ERR_INVALID_BOUNDARY;
      }
    } else {
      job->is_start = false;
    }

    Jsonv_Value v_end;
    if (jsonv_obj_get(job_spec_val.as.p, "end", &v_end)) {
      if (v_end.tag == JSONV_VAL_BOOLEAN) {
        job->is_end = v_end.as.boolean;
      } else {
        return ERR_INVALID_BOUNDARY;
      }
    } else {
      job->is_end = false;
    }

    Jsonv_Value v_return;
    if (jsonv_obj_get(job_spec_val.as.p, "return", &v_return)) {
      if (v_return.tag == JSONV_VAL_STRING) {
        job->return_expr.data = v_return.as.p;
        job->return_expr.length = jsonv_val_str_len(v_return);
      } else {
        char *serialized_str = NULL;
        int32_t ser_status = serialize_jsonv_value(arena, v_return, &serialized_str);
        if (ser_status != ERR_SUCCESS) {
          return ser_status;
        }
        job->return_expr.data = serialized_str;
        job->return_expr.length = strlen(serialized_str);
      }
    } else {
      job->return_expr.data = NULL;
      job->return_expr.length = 0;
    }

    // Parse Cache configuration
    job->cache_enabled = true;
    job->has_cache_ttl = false;
    job->cache_ttl = 0;
    Jsonv_Value v_cache;
    if (jsonv_obj_get(job_spec_val.as.p, "cache", &v_cache)) {
      if (v_cache.tag == JSONV_VAL_BOOLEAN) {
        job->cache_enabled = v_cache.as.boolean;
      } else if (v_cache.tag == JSONV_VAL_OBJ) {
        Jsonv_Value v_enabled;
        if (jsonv_obj_get(v_cache.as.p, "enabled", &v_enabled)) {
          if (v_enabled.tag == JSONV_VAL_BOOLEAN) {
            job->cache_enabled = v_enabled.as.boolean;
          }
        }
        Jsonv_Value v_ttl;
        if (jsonv_obj_get(v_cache.as.p, "ttl", &v_ttl)) {
          if (v_ttl.tag == JSONV_VAL_INT) {
            job->has_cache_ttl = true;
            job->cache_ttl = (int32_t)v_ttl.as.i;
          } else if (v_ttl.tag == JSONV_VAL_STRING) {
            const char *ttl_str = v_ttl.as.p;
            size_t ttl_len = jsonv_val_str_len(v_ttl);
            int32_t multiplier = 1;
            if (ttl_len > 1) {
              char unit = ttl_str[ttl_len - 1];
              if (unit == 's' || unit == 'S') multiplier = 1;
              else if (unit == 'm' || unit == 'M') multiplier = 60;
              else if (unit == 'h' || unit == 'H') multiplier = 3600;
              else if (unit == 'd' || unit == 'D') multiplier = 86400;
            }
            int32_t val = 0;
            size_t digits = (ttl_len > 1 && (ttl_str[ttl_len - 1] == 's' || ttl_str[ttl_len - 1] == 'S' ||
                                              ttl_str[ttl_len - 1] == 'm' || ttl_str[ttl_len - 1] == 'M' ||
                                              ttl_str[ttl_len - 1] == 'h' || ttl_str[ttl_len - 1] == 'H' ||
                                              ttl_str[ttl_len - 1] == 'd' || ttl_str[ttl_len - 1] == 'D'))
                            ? ttl_len - 1 : ttl_len;
            for (size_t d = 0; d < digits; d++) {
              if (ttl_str[d] >= '0' && ttl_str[d] <= '9') {
                val = val * 10 + (ttl_str[d] - '0');
              }
            }
            job->has_cache_ttl = true;
            job->cache_ttl = val * multiplier;
          }
        }
      }
    }

    // Extract Type
    Jsonv_Value v_type;
    NodeType type = NODE_TASK;
    if (jsonv_obj_get(job_spec_val.as.p, "type", &v_type) && v_type.tag == JSONV_VAL_STRING) {
      const char *type_cstr = v_type.as.p;
      size_t type_len = jsonv_val_str_len(v_type);
      StringView type_sv = { type_cstr, type_len };

      if (sv_equals_cstr(type_sv, "task")) type = NODE_TASK;
      else if (sv_equals_cstr(type_sv, "if")) type = NODE_IF;
      else if (sv_equals_cstr(type_sv, "switch")) type = NODE_SWITCH;
      else if (sv_equals_cstr(type_sv, "fork")) type = NODE_FORK;
      else if (sv_equals_cstr(type_sv, "join")) type = NODE_JOIN;
      else if (sv_equals_cstr(type_sv, "loop")) type = NODE_LOOP;
      else if (sv_equals_cstr(type_sv, "wait_signal")) type = NODE_WAIT_SIGNAL;
      else if (sv_equals_cstr(type_sv, "wait_timer")) type = NODE_WAIT_TIMER;
      else if (sv_equals_cstr(type_sv, "transform")) type = NODE_TRANSFORM;
      else if (sv_equals_cstr(type_sv, "export")) type = NODE_EXPORT;
      else return ERR_MISSING_VAR; // Unknown node type
    }
    job->type = type;

    // Extract Name
    Jsonv_Value v_name;
    if (jsonv_obj_get(job_spec_val.as.p, "name", &v_name) && v_name.tag == JSONV_VAL_STRING) {
      job->name.data = v_name.as.p;
      job->name.length = jsonv_val_str_len(v_name);
    } else {
      job->name = job->id;
    }

    // Extract depends_on
    Jsonv_Value v_dep;
    if (jsonv_obj_get(job_spec_val.as.p, "depends_on", &v_dep)) {
      bool has_object_dep = false;
      if (v_dep.tag == JSONV_VAL_ARRAY) {
        int dep_count = jsonv_arr_length(v_dep.as.p);
        for (int d = 0; d < dep_count; d++) {
          if (jsonv_arr_val_at(v_dep.as.p, d).tag == JSONV_VAL_OBJ) {
            has_object_dep = true;
            break;
          }
        }
      } else if (v_dep.tag == JSONV_VAL_OBJ) {
        has_object_dep = true;
      }

      if (v_dep.tag == JSONV_VAL_ARRAY) {
        int dep_count = jsonv_arr_length(v_dep.as.p);
        job->dependency_count = dep_count;
        if (dep_count > 0) {
          job->depends_on_ids = na_alloc(arena, dep_count * sizeof(StringView));
          job->depends_on_nodes = na_alloc(arena, dep_count * sizeof(JobNode *));
          if (has_object_dep) {
            job->depends_on_conditions = na_alloc(arena, dep_count * sizeof(uint8_t));
            if (!job->depends_on_conditions) return ERR_OOM;
          } else {
            job->depends_on_conditions = NULL;
          }
          if (!job->depends_on_ids || !job->depends_on_nodes)
            return ERR_OOM;
          for (int d = 0; d < dep_count; d++) {
            Jsonv_Value dep_item = jsonv_arr_val_at(v_dep.as.p, d);
            if (dep_item.tag == JSONV_VAL_STRING) {
              job->depends_on_ids[d].data = dep_item.as.p;
              job->depends_on_ids[d].length = jsonv_val_str_len(dep_item);
              job->depends_on_nodes[d] = NULL;
              if (job->depends_on_conditions) {
                job->depends_on_conditions[d] = DEP_COND_SUCCESS;
              }
            } else if (dep_item.tag == JSONV_VAL_OBJ) {
              Jsonv_Value v_job_id;
              if (!jsonv_obj_get(dep_item.as.p, "job", &v_job_id) || v_job_id.tag != JSONV_VAL_STRING)
                return ERR_MISSING_VAR;
              job->depends_on_ids[d].data = v_job_id.as.p;
              job->depends_on_ids[d].length = jsonv_val_str_len(v_job_id);
              job->depends_on_nodes[d] = NULL;

              uint8_t mask = 0;
              Jsonv_Value v_conds;
              if (jsonv_obj_get(dep_item.as.p, "conditions", &v_conds) && v_conds.tag == JSONV_VAL_ARRAY) {
                int cond_len = jsonv_arr_length(v_conds.as.p);
                for (int c = 0; c < cond_len; c++) {
                  Jsonv_Value cond_val = jsonv_arr_val_at(v_conds.as.p, c);
                  if (cond_val.tag == JSONV_VAL_STRING) {
                    StringView cond_sv = { cond_val.as.p, jsonv_val_str_len(cond_val) };
                    if (sv_equals_cstr(cond_sv, "onSuccess")) {
                      mask |= DEP_COND_SUCCESS;
                    } else if (sv_equals_cstr(cond_sv, "onFailure")) {
                      mask |= DEP_COND_FAILURE;
                    } else if (sv_equals_cstr(cond_sv, "onSkip")) {
                      mask |= DEP_COND_SKIP;
                    } else if (sv_equals_cstr(cond_sv, "onCompletion")) {
                      mask |= DEP_COND_COMPLETION;
                    } else {
                      return ERR_MISSING_VAR;
                    }
                  } else {
                    return ERR_MISSING_VAR;
                  }
                }
                if (mask == 0) {
                  mask = DEP_COND_SUCCESS;
                }
              } else {
                mask = DEP_COND_SUCCESS;
              }
              if (job->depends_on_conditions) {
                job->depends_on_conditions[d] = mask;
              }
            } else {
              return ERR_MISSING_VAR;
            }
          }
        }
      } else if (v_dep.tag == JSONV_VAL_STRING) {
        job->dependency_count = 1;
        job->depends_on_ids = na_alloc(arena, sizeof(StringView));
        job->depends_on_nodes = na_alloc(arena, sizeof(JobNode *));
        job->depends_on_conditions = NULL;
        if (!job->depends_on_ids || !job->depends_on_nodes)
          return ERR_OOM;
        job->depends_on_ids[0].data = v_dep.as.p;
        job->depends_on_ids[0].length = jsonv_val_str_len(v_dep);
        job->depends_on_nodes[0] = NULL;
      } else if (v_dep.tag == JSONV_VAL_OBJ) {
        job->dependency_count = 1;
        job->depends_on_ids = na_alloc(arena, sizeof(StringView));
        job->depends_on_nodes = na_alloc(arena, sizeof(JobNode *));
        job->depends_on_conditions = na_alloc(arena, sizeof(uint8_t));
        if (!job->depends_on_ids || !job->depends_on_nodes || !job->depends_on_conditions)
          return ERR_OOM;

        Jsonv_Value v_job_id;
        if (!jsonv_obj_get(v_dep.as.p, "job", &v_job_id) || v_job_id.tag != JSONV_VAL_STRING)
          return ERR_MISSING_VAR;
        job->depends_on_ids[0].data = v_job_id.as.p;
        job->depends_on_ids[0].length = jsonv_val_str_len(v_job_id);
        job->depends_on_nodes[0] = NULL;

        uint8_t mask = 0;
        Jsonv_Value v_conds;
        if (jsonv_obj_get(v_dep.as.p, "conditions", &v_conds) && v_conds.tag == JSONV_VAL_ARRAY) {
          int cond_len = jsonv_arr_length(v_conds.as.p);
          for (int c = 0; c < cond_len; c++) {
            Jsonv_Value cond_val = jsonv_arr_val_at(v_conds.as.p, c);
            if (cond_val.tag == JSONV_VAL_STRING) {
              StringView cond_sv = { cond_val.as.p, jsonv_val_str_len(cond_val) };
              if (sv_equals_cstr(cond_sv, "onSuccess")) {
                mask |= DEP_COND_SUCCESS;
              } else if (sv_equals_cstr(cond_sv, "onFailure")) {
                mask |= DEP_COND_FAILURE;
              } else if (sv_equals_cstr(cond_sv, "onSkip")) {
                mask |= DEP_COND_SKIP;
              } else if (sv_equals_cstr(cond_sv, "onCompletion")) {
                mask |= DEP_COND_COMPLETION;
              } else {
                return ERR_MISSING_VAR;
              }
            } else {
              return ERR_MISSING_VAR;
            }
          }
          if (mask == 0) {
            mask = DEP_COND_SUCCESS;
          }
        } else {
          mask = DEP_COND_SUCCESS;
        }
        job->depends_on_conditions[0] = mask;
      } else {
        return ERR_MISSING_VAR;
      }
    } else {
      job->dependency_count = 0;
      job->depends_on_ids = NULL;
      job->depends_on_nodes = NULL;
      job->depends_on_conditions = NULL;
    }

    // Extract Type-Specific Specs
    int32_t spec_status = ERR_SUCCESS;
    if (job->type == NODE_TASK) {
      spec_status = compile_task_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_IF) {
      spec_status = compile_if_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_SWITCH) {
      spec_status = compile_switch_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_FORK) {
      spec_status = compile_fork_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_JOIN) {
      spec_status = compile_join_spec(job, job_spec_val);
    } else if (job->type == NODE_LOOP) {
      spec_status = compile_loop_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_WAIT_SIGNAL) {
      spec_status = compile_wait_signal_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_WAIT_TIMER) {
      spec_status = compile_wait_timer_spec(job, job_spec_val);
    } else if (job->type == NODE_TRANSFORM) {
      spec_status = compile_transform_spec(arena, job, job_spec_val);
    } else if (job->type == NODE_EXPORT) {
      spec_status = compile_export_spec(arena, job, job_spec_val);
    }
    if (spec_status != ERR_SUCCESS) {
      return spec_status;
    }

    job_nodes[i] = job;
  }

  // 1.5. Inject implicit dependencies from control flow nodes (if, switch, fork).
  int32_t dep_status = inject_implicit_dependencies(arena, job_nodes, job_count);
  if (dep_status != ERR_SUCCESS) return dep_status;

  // 2. Resolve dependency nodes
  dep_status = resolve_dependencies(job_nodes, job_count);
  if (dep_status != ERR_SUCCESS) return dep_status;

  // 2.5. Deterministic Boundary Validation
  int32_t boundary_status = validate_boundaries(arena, job_nodes, job_count);
  if (boundary_status != ERR_SUCCESS) return boundary_status;

  // 3. Kahn's Algorithm for Topological Sorting & Cycle Detection
  JobNode *sorted_head = NULL;
  size_t sorted_count = 0;
  int32_t sort_status = topological_sort(arena, job_nodes, job_count, &sorted_head, &sorted_count);
  if (sort_status != ERR_SUCCESS) return sort_status;

  ast->jobs_head = sorted_head;
  ast->job_count = sorted_count;

  return ERR_SUCCESS;
  /*#endregion*/
}
