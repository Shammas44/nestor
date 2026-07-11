#include "testutils.h"
#include "arena.h"
#include "parser.h"
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

TIMED_TEST(stage2, parse_valid_yaml, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml = 
    "version: 2.0.0\n"
    "name: Test Onboarding Pipeline\n"
    "on: { manual: {} }\n"
    "env:\n"
    "  GLOBAL_URL: \"https://api.internal\"\n"
    "  TIMEOUT: \"30s\"\n"
    "jobs: {}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  // Verify fields
  cr_assert(sv_equals_cstr(ast.version, "2.0.0"));
  cr_assert(sv_equals_cstr(ast.name, "Test Onboarding Pipeline"));

  // Verify env variables
  cr_assert_eq(ast.env_count, 2);
  cr_assert(sv_equals_cstr(ast.env[0].key, "GLOBAL_URL"));
  cr_assert(sv_equals_cstr(ast.env[0].value, "https://api.internal"));
  cr_assert(sv_equals_cstr(ast.env[1].key, "TIMEOUT"));
  cr_assert(sv_equals_cstr(ast.env[1].value, "30s"));

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage2, parse_invalid_semver, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml = 
    "version: 2.0\n" // invalid semver
    "name: Test Pipeline\n"
    "on: { manual: {} }\n"
    "jobs: {}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert(status != ERR_SUCCESS);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
