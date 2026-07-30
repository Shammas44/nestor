#include "loader.h"
#include "parser.h"
#include <jsonv/ctx.h>
#include <jsonv/shape.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void *loader_jsonv_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void loader_jsonv_reset(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static void loader_jsonv_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  (void)user_data;
  (void)keep_size;
  /*#endregion*/
}

static void loader_jsonv_destroy(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static const Jsonv_Arena_Ops loader_jsonv_ops = {
  .alloc = loader_jsonv_alloc,
  .reset = loader_jsonv_reset,
  .reset_to = loader_jsonv_reset_to,
  .destroy = loader_jsonv_destroy
};

static char *read_file_content(Arena *arena, const char *filepath, size_t *out_len) {
  /*#region*/
  FILE *f = fopen(filepath, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)na_alloc(arena, size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, size, f);
  buf[n] = '\0';
  fclose(f);
  if (out_len) *out_len = n;
  return buf;
  /*#endregion*/
}

WorkflowDef *workspace_find_workflow(WorkspaceMap *map, StringView name) {
  /*#region*/
  if (!map) return NULL;
  WorkflowDef *curr = map->workflows_head;
  while (curr) {
    if (sv_compare(curr->name, name) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
  /*#endregion*/
}

ProviderDef *workspace_find_provider(WorkspaceMap *map, StringView name) {
  /*#region*/
  if (!map) return NULL;
  ProviderDef *curr = map->providers_head;
  while (curr) {
    if (sv_compare(curr->name, name) == 0) {
      return curr;
    }
    curr = curr->next;
  }
  return NULL;
  /*#endregion*/
}

static int32_t dfs_check_cycle(WorkspaceMap *map, WorkflowDef *wf, WorkflowDef **stack, size_t stack_depth) {
  /*#region*/
  if (!wf) return ERR_SUCCESS;

  for (size_t i = 0; i < stack_depth; i++) {
    if (stack[i] == wf) {
      return ERR_CYCLIC_DEP;
    }
  }

  stack[stack_depth] = wf;

  for (size_t c = 0; c < wf->call_count; c++) {
    StringView call_target = wf->calls[c];
    // Remove "workflows." prefix if present
    if (sv_starts_with(call_target, (StringView){"workflows.", 10})) {
      call_target.data += 10;
      call_target.length -= 10;
    }
    WorkflowDef *target_wf = workspace_find_workflow(map, call_target);
    if (target_wf) {
      int32_t res = dfs_check_cycle(map, target_wf, stack, stack_depth + 1);
      if (res != ERR_SUCCESS) return res;
    }
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t workspace_check_cycles(WorkspaceMap *map) {
  /*#region*/
  if (!map || map->workflow_count == 0) return ERR_SUCCESS;

  WorkflowDef **stack = (WorkflowDef **)na_alloc(NULL, sizeof(WorkflowDef *) * (map->workflow_count + 1));
  // Allocate on arena or stack array if map->workflow_count fits
  WorkflowDef *visited_stack[256];
  WorkflowDef **dyn_stack = visited_stack;
  if (map->workflow_count > 256) {
    (void)stack; // suppress unused warning
    return ERR_OOM;
  }

  WorkflowDef *curr = map->workflows_head;
  while (curr) {
    int32_t res = dfs_check_cycle(map, curr, dyn_stack, 0);
    if (res != ERR_SUCCESS) return res;
    curr = curr->next;
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

static void scrape_workflow_calls(Arena *arena, WorkflowAST *ast, StringView **out_calls, size_t *out_count) {
  /*#region*/
  size_t cap = 16;
  size_t count = 0;
  StringView *calls = (StringView *)na_alloc(arena, sizeof(StringView) * cap);
  if (!calls) {
    *out_calls = NULL;
    *out_count = 0;
    return;
  }

  JobNode *job = ast->jobs_head;
  while (job) {
    if (job->type == NODE_TASK) {
      StepNode *step = job->spec.task.steps_head;
      while (step) {
        if (!step->is_http && step->plugin.uses.length > 0) {
          if (count >= cap) {
            cap *= 2;
            StringView *new_calls = (StringView *)na_alloc(arena, sizeof(StringView) * cap);
            if (new_calls) {
              memcpy(new_calls, calls, sizeof(StringView) * count);
              calls = new_calls;
            }
          }
          if (count < cap) {
            calls[count++] = step->plugin.uses;
          }
        }
        step = step->next;
      }
    } else if (job->type == NODE_LOOP) {
      StepNode *step = job->spec.loop_node.steps_head;
      while (step) {
        if (!step->is_http && step->plugin.uses.length > 0) {
          if (count >= cap) {
            cap *= 2;
            StringView *new_calls = (StringView *)na_alloc(arena, sizeof(StringView) * cap);
            if (new_calls) {
              memcpy(new_calls, calls, sizeof(StringView) * count);
              calls = new_calls;
            }
          }
          if (count < cap) {
            calls[count++] = step->plugin.uses;
          }
        }
        step = step->next;
      }
    }
    job = job->next_sorted;
  }

  *out_calls = calls;
  *out_count = count;
  /*#endregion*/
}

static int32_t load_file_into_workspace(Arena *arena, const char *filepath, WorkspaceMap *map) {
  /*#region*/
  size_t content_len = 0;
  char *content = read_file_content(arena, filepath, &content_len);
  if (!content || content_len == 0) return ERR_SUCCESS;

  // Check if provider contract YAML (key 'provider:') first to avoid spurious validation errors
  char *provider_kw = strstr(content, "provider:");
  if (provider_kw) {
    ProviderDef *pdef = (ProviderDef *)na_alloc(arena, sizeof(ProviderDef));
    if (!pdef) return ERR_OOM;
    memset(pdef, 0, sizeof(ProviderDef));

    Jsonv_Arena *jarena = jsonv_arena_new_custom(&loader_jsonv_ops, arena);
    Jsonv_Context *ctx = jsonv_ctx_new(jarena, NULL, NULL);
    if (ctx && jsonv_ctx_parse_yaml_data(ctx, (const unsigned char *)content)) {
      Jsonv_Value root;
      jsonv_ctx_get_value(ctx, &root);
      if (root.tag == JSONV_VAL_OBJ) {
        Jsonv_Value name_val;
        if (jsonv_obj_get(root.as.p, "provider", &name_val) && name_val.tag == JSONV_VAL_STRING) {
          pdef->name.data = name_val.as.p;
          pdef->name.length = jsonv_val_str_len(name_val);
        }
        Jsonv_Value desc_val;
        if (jsonv_obj_get(root.as.p, "description", &desc_val) && desc_val.tag == JSONV_VAL_STRING) {
          pdef->description.data = desc_val.as.p;
          pdef->description.length = jsonv_val_str_len(desc_val);
        }
        Jsonv_Value ops_val;
        if (jsonv_obj_get(root.as.p, "operations", &ops_val) && ops_val.tag == JSONV_VAL_OBJ) {
          pdef->operations = ops_val;
        } else {
          pdef->operations = jsonv_val_obj(jsonv_obj_new(jarena, NULL));
        }
      }
    }

    if (pdef->name.length == 0) {
      char *name_start = provider_kw + 9;
      while (*name_start == ' ' || *name_start == '\t') name_start++;
      char *name_end = name_start;
      while (*name_end && *name_end != '\r' && *name_end != '\n') name_end++;
      pdef->name = (StringView){ name_start, (size_t)(name_end - name_start) };
    }

    pdef->next = map->providers_head;
    map->providers_head = pdef;
    map->provider_count++;
    return ERR_SUCCESS;
  }

  // Attempt parsing as Workflow
  WorkflowDef *wf = (WorkflowDef *)na_alloc(arena, sizeof(WorkflowDef));
  if (!wf) return ERR_OOM;
  memset(wf, 0, sizeof(WorkflowDef));
  wf->file_path = (char *)filepath;

  int32_t parse_res = parser_parse_buffer(arena, content, content_len, &wf->ast);
  if (parse_res == ERR_SUCCESS && wf->ast.name.length > 0) {
    wf->name = wf->ast.name;
    scrape_workflow_calls(arena, &wf->ast, &wf->calls, &wf->call_count);
    wf->next = map->workflows_head;
    map->workflows_head = wf;
    map->workflow_count++;
    if (!map->root_workflow) map->root_workflow = wf;
    return ERR_SUCCESS;
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

static void crawl_dir(Arena *arena, const char *dir_path, WorkspaceMap *map) {
  /*#region*/
  DIR *d = opendir(dir_path);
  if (!d) return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

    struct stat st;
    if (stat(full_path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        crawl_dir(arena, full_path, map);
      } else if (S_ISREG(st.st_mode)) {
        size_t len = strlen(full_path);
        if ((len > 5 && strcmp(full_path + len - 5, ".yaml") == 0) ||
            (len > 4 && strcmp(full_path + len - 4, ".yml") == 0)) {
          char *saved_path = (char *)na_alloc(arena, len + 1);
          if (saved_path) {
            strcpy(saved_path, full_path);
            load_file_into_workspace(arena, saved_path, map);
          }
        }
      }
    }
  }
  closedir(d);
  /*#endregion*/
}

int32_t workspace_load_directory(Arena *arena, const char *project_dir, WorkspaceMap *out_map) {
  /*#region*/
  if (!project_dir || !out_map) return ERR_INVALID_BOUNDARY;
  memset(out_map, 0, sizeof(WorkspaceMap));

  crawl_dir(arena, project_dir, out_map);

  return workspace_check_cycles(out_map);
  /*#endregion*/
}
