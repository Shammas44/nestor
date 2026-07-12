#include "testutils.h"
#include "arena.h"
#include "parser.h"
#include "evaluator.h"
#include <criterion/criterion.h>
#include <string.h>

static inline StringView sv_from_cstr(const char *s) {
  StringView sv;
  sv.data = s;
  sv.length = strlen(s);
  return sv;
}

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

TIMED_TEST(stage4, basic_expression_evaluation, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "version: 2.0.0\n"
    "name: Expression Test\n"
    "on: { manual: {} }\n"
    "inputs:\n"
    "  user_id: Sebastien\n"
    "env:\n"
    "  ENV_VAR: production\n"
    "jobs: {}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);

  // Expression: ${{ inputs.user_id & '-' & env.ENV_VAR }}
  StringView expr = sv_from_cstr("${{ inputs.user_id & '-' & env.ENV_VAR }}");
  Jsonv_Value out_val = jsonv_val_undefined();
  int32_t eval_status = evaluate_expression(arena, expr, jsonv_arena, ast.root_val, &out_val);
  cr_assert_eq(eval_status, ERR_SUCCESS);

  cr_assert_eq(out_val.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){out_val.as.p, jsonv_val_str_len(out_val)}, "Sebastien-production"));

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage4, expression_whitespace_and_boundaries, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "version: 2.0.0\n"
    "name: Expression Test\n"
    "on: { manual: {} }\n"
    "inputs:\n"
    "  user_id: Sebastien\n"
    "env:\n"
    "  ENV_VAR: production\n"
    "jobs: {}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);

  // Expression: ${{  inputs.user_id   }}
  StringView expr = sv_from_cstr("${{  inputs.user_id   }}");
  Jsonv_Value out_val = jsonv_val_undefined();
  int32_t eval_status = evaluate_expression(arena, expr, jsonv_arena, ast.root_val, &out_val);
  cr_assert_eq(eval_status, ERR_SUCCESS);

  cr_assert_eq(out_val.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){out_val.as.p, jsonv_val_str_len(out_val)}, "Sebastien"));

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage4, missing_property_evaluation, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "version: 2.0.0\n"
    "name: Expression Test\n"
    "on: { manual: {} }\n"
    "inputs:\n"
    "  user_id: Sebastien\n"
    "env:\n"
    "  ENV_VAR: production\n"
    "jobs: {}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);

  // Expression targeting non-existent property
  StringView expr = sv_from_cstr("${{ inputs.non_existent }}");
  Jsonv_Value out_val = jsonv_val_undefined();
  int32_t eval_status = evaluate_expression(arena, expr, jsonv_arena, ast.root_val, &out_val);
  cr_assert_eq(eval_status, ERR_MISSING_VAR);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
