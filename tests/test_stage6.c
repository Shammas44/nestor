#include "testutils.h"
#include "nestor.h"
#include "parser.h"
#include "compiler.h"
#include <criterion/criterion.h>
#include <string.h>
#include <stdio.h>

static void init() {
  /*#region*/
  test_init();
  // Ensure the mock plugin is built
  system("mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin");
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  /*#endregion*/
}

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

static void assert_job_state(WorkflowAST *ast, const char *id, JobState expected_state) {
  /*#region*/
  JobNode *curr = ast->jobs_head;
  while (curr) {
    if (sv_equals_cstr(curr->id, id)) {
      cr_assert_eq(curr->execution_state, expected_state, "Job %s: expected state %d, got %d", id, expected_state, curr->execution_state);
      return;
    }
    curr = curr->next_sorted;
  }
  cr_assert_fail("Job %s not found in AST", id);
  /*#endregion*/
}

TIMED_TEST(stage6, if_node_then_branch, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"If Then Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"check_condition\": {\n"
    "      \"type\": \"if\",\n"
    "      \"condition\": \"${{ inputs.val = 1 }}\",\n"
    "      \"then\": [\"then_job\"],\n"
    "      \"else\": [\"else_job\"]\n"
    "    },\n"
    "    \"then_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step1\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"val\": \"then\" }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"else_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step2\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"val\": \"else\" }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "val"), jsonv_val_int(1));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "check_condition", STATE_SUCCEEDED);
  assert_job_state(&ast, "then_job", STATE_SUCCEEDED);
  assert_job_state(&ast, "else_job", STATE_SKIPPED);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, if_node_else_branch, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"If Else Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"check_condition\": {\n"
    "      \"type\": \"if\",\n"
    "      \"condition\": \"${{ inputs.val = 1 }}\",\n"
    "      \"then\": [\"then_job\"],\n"
    "      \"else\": [\"else_job\"]\n"
    "    },\n"
    "    \"then_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step1\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"val\": \"then\" }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"else_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step2\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"val\": \"else\" }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "val"), jsonv_val_int(2));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "check_condition", STATE_SUCCEEDED);
  assert_job_state(&ast, "then_job", STATE_SKIPPED);
  assert_job_state(&ast, "else_job", STATE_SUCCEEDED);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, switch_node_cases, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Switch Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"check_switch\": {\n"
    "      \"type\": \"switch\",\n"
    "      \"cases\": [\n"
    "        {\n"
    "          \"condition\": \"${{ inputs.val = 'a' }}\",\n"
    "          \"then\": [\"job_a\"]\n"
    "        },\n"
    "        {\n"
    "          \"condition\": \"${{ inputs.val = 'b' }}\",\n"
    "          \"then\": [\"job_b\"]\n"
    "        }\n"
    "      ],\n"
    "      \"default\": [\"job_default\"]\n"
    "    },\n"
    "    \"job_a\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        { \"id\": \"s\", \"uses\": \"./plugins/mock_plugin\", \"with\": {} }\n"
    "      ]\n"
    "    },\n"
    "    \"job_b\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        { \"id\": \"s\", \"uses\": \"./plugins/mock_plugin\", \"with\": {} }\n"
    "      ]\n"
    "    },\n"
    "    \"job_default\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        { \"id\": \"s\", \"uses\": \"./plugins/mock_plugin\", \"with\": {} }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "val"), jsonv_val_str(allocate_jsonv_string(arena, "b")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "check_switch", STATE_SUCCEEDED);
  assert_job_state(&ast, "job_a", STATE_SKIPPED);
  assert_job_state(&ast, "job_b", STATE_SUCCEEDED);
  assert_job_state(&ast, "job_default", STATE_SKIPPED);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, loop_while_iteration, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Loop While Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"while_loop\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"while\",\n"
    "      \"condition\": \"${{ index < 3 }}\",\n"
    "      \"max_iterations\": 5,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"loop_step\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"idx\": \"${{ index }}\" }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "while_loop", STATE_SUCCEEDED);

  // Check that index ended at 3 (since index < 3 evaluates to false at index = 3, ending loop)
  Jsonv_Value index_val;
  cr_assert(jsonv_obj_get(root_obj, allocate_jsonv_string(arena, "index"), &index_val));
  cr_assert_eq(index_val.tag, JSONV_VAL_INT);
  cr_assert_eq(index_val.as.i, 3);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, loop_while_overflow, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Loop Overflow Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"overflow_loop\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"while\",\n"
    "      \"condition\": \"${{ index < 10 }}\",\n"
    "      \"max_iterations\": 3,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"s\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": {}\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_LOOP_MAX_ITERATIONS);

  assert_job_state(&ast, "overflow_loop", STATE_FAILED);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, loop_for_each, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Loop For Each Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"for_each_loop\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"for_each\",\n"
    "      \"items\": \"${{ ['x', 'y', 'z'] }}\",\n"
    "      \"max_iterations\": 5,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"foreach_step\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": { \"val\": \"${{ item }}\" }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "for_each_loop", STATE_SUCCEEDED);

  // Check last item bound
  Jsonv_Value item_val;
  cr_assert(jsonv_obj_get(root_obj, allocate_jsonv_string(arena, "item"), &item_val));
  cr_assert_eq(item_val.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){item_val.as.p, jsonv_val_str_len(item_val)}, "z"));

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage6, join_node_strategies, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Join Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"start_job\": {\n"
    "      \"type\": \"if\",\n"
    "      \"condition\": \"${{ inputs.val = 1 }}\",\n"
    "      \"then\": [\"job_then\"],\n"
    "      \"else\": [\"job_else\"]\n"
    "    },\n"
    "    \"job_then\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [{ \"id\": \"s\", \"uses\": \"./plugins/mock_plugin\", \"with\": {} }]\n"
    "    },\n"
    "    \"job_else\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [{ \"id\": \"s\", \"uses\": \"./plugins/mock_plugin\", \"with\": {} }]\n"
    "    },\n"
    "    \"join_all\": {\n"
    "      \"type\": \"join\",\n"
    "      \"depends_on\": [\"job_then\", \"job_else\"],\n"
    "      \"strategy\": \"all\"\n"
    "    },\n"
    "    \"join_any\": {\n"
    "      \"type\": \"join\",\n"
    "      \"depends_on\": [\"job_then\", \"job_else\"],\n"
    "      \"strategy\": \"any\"\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "val"), jsonv_val_int(1));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  if (run_status != ERR_SUCCESS) {
    printf("DEBUG: run_status = %d\n", run_status);
    JobNode *j = ast.jobs_head;
    while (j) {
      printf("DEBUG: JOB %.*s: state=%d\n", (int)j->id.length, j->id.data, j->execution_state);
      j = j->next_sorted;
    }
  }
  cr_assert_eq(run_status, ERR_SUCCESS);

  assert_job_state(&ast, "start_job", STATE_SUCCEEDED);
  assert_job_state(&ast, "job_then", STATE_SUCCEEDED);
  assert_job_state(&ast, "job_else", STATE_SKIPPED);
  assert_job_state(&ast, "join_all", STATE_SUCCEEDED);
  assert_job_state(&ast, "join_any", STATE_SUCCEEDED);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
