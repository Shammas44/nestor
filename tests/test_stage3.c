#include "testutils.h"
#include "arena.h"
#include "parser.h"
#include "compiler.h"
#include <criterion/criterion.h>
#include <string.h>

static void init() {
  /*#region*/
  test_init();
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  /*#endregion*/
}

TIMED_TEST(stage3, cyclic_dependency_detection, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  WorkflowAST ast;
  int32_t status = parser_parse_file(arena, "tests/fixtures/cyclic.yaml", &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_CYCLIC_DEP, "Expected ERR_CYCLIC_DEP (-2) but got %d", compile_status);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage3, topological_sorting, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  WorkflowAST ast;
  int32_t status = parser_parse_file(arena, "tests/fixtures/multi_branch.yaml", &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  // Check the topologically sorted jobs head and order
  cr_assert_eq(ast.job_count, 4);

  JobNode *j1 = ast.jobs_head;
  cr_assert_not_null(j1);
  cr_assert(sv_equals_cstr(j1->id, "validate_payload"));

  JobNode *j2 = j1->next_sorted;
  cr_assert_not_null(j2);
  cr_assert(sv_equals_cstr(j2->id, "check_tier"));

  JobNode *j3 = j2->next_sorted;
  cr_assert_not_null(j3);
  cr_assert(sv_equals_cstr(j3->id, "deploy_standard"));

  JobNode *j4 = j3->next_sorted;
  cr_assert_not_null(j4);
  cr_assert(sv_equals_cstr(j4->id, "notify_success"));
  cr_assert_null(j4->next_sorted);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage3, missing_dependency_validation, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml = 
    "version: 2.0.0\n"
    "name: Missing Dependency Test\n"
    "on: { manual: {} }\n"
    "jobs:\n"
    "  job_a:\n"
    "    type: task\n"
    "    depends_on: [non_existent_job]\n"
    "    steps: []\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_MISSING_VAR, "Expected ERR_MISSING_VAR but got %d", compile_status);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
