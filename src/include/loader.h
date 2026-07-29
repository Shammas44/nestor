#ifndef _NESTOR_LOADER_H
#define _NESTOR_LOADER_H

#include "arena.h"
#include "stringview.h"
#include "ast.h"
#include "provider.h"
#include "error_codes.h"

typedef struct ProviderDef ProviderDef;
struct ProviderDef {
  StringView name;
  StringView description;
  Jsonv_Value operations; // Jsonv Object mapping operation_name -> {inputs, outputs}
  ProviderDef *next;
};

typedef struct WorkflowDef WorkflowDef;
struct WorkflowDef {
  StringView name;
  WorkflowAST ast;
  char *file_path;
  StringView *calls; // array of sub-workflow names called by this workflow
  size_t call_count;
  WorkflowDef *next;
};

typedef struct WorkspaceMap {
  ProviderDef *providers_head;
  size_t provider_count;

  WorkflowDef *workflows_head;
  size_t workflow_count;

  WorkflowDef *root_workflow;
} WorkspaceMap;

int32_t workspace_load_directory(Arena *arena, const char *project_dir, WorkspaceMap *out_map);
WorkflowDef *workspace_find_workflow(WorkspaceMap *map, StringView name);
ProviderDef *workspace_find_provider(WorkspaceMap *map, StringView name);
int32_t workspace_check_cycles(WorkspaceMap *map);

#endif // _NESTOR_LOADER_H
