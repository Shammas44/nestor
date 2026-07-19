#include "testutils.h"
#include "nestor.h"
#include "parser.h"
#include "compiler.h"
#include <criterion/criterion.h>
#include <stdio.h>
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

#define CHUNK_SIZE 4096
static char *read_stdin_to_buffer(Arena *arena, size_t *out_len) {
  /*#region*/
  size_t capacity = CHUNK_SIZE;
  size_t length = 0;
  char *buf = na_alloc(arena, capacity);
  if (!buf) return NULL;

  while (1) {
    size_t n = fread(buf + length, 1, CHUNK_SIZE, stdin);
    if (n == 0) break;
    length += n;
    if (length + CHUNK_SIZE > capacity) {
      capacity *= 2;
      char *new_buf = na_alloc(arena, capacity);
      if (!new_buf) return NULL;
      memcpy(new_buf, buf, length);
      buf = new_buf;
    }
  }
  buf[length] = '\0';
  *out_len = length;
  return buf;
  /*#endregion*/
}

static void run_example_test(const char *filepath) {
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Redirect stdin to the file
  FILE *f = freopen(filepath, "r", stdin);
  cr_assert_not_null(f);

  size_t len = 0;
  char *yaml = read_stdin_to_buffer(arena, &len);
  cr_assert_not_null(yaml);
  cr_assert(len > 0);

  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, len, &ast);
  cr_assert_eq(parse_status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  arena_destroy(arena);
  /*#endregion*/
}

TIMED_TEST(examples, basic_http, init, fini)
/*#region*/
  run_example_test("examples/01_basic_http.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, plugin_pipeline, init, fini)
/*#region*/
  run_example_test("examples/02_plugin_pipeline.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, enterprise_deploy, init, fini)
/*#region*/
  run_example_test("examples/03_enterprise_deploy.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, multistep_jsonata, init, fini)
/*#region*/
  run_example_test("examples/04_multistep_jsonata.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, concurrency_resume, init, fini)
/*#region*/
  run_example_test("examples/05_concurrency_resume.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, secrets_redaction, init, fini)
/*#region*/
  run_example_test("examples/06_secrets_redaction.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, conditionals, init, fini)
/*#region*/
  run_example_test("examples/07_conditionals.json");
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, loops, init, fini)
/*#region*/
  run_example_test("examples/08_loops.json");
/*#endregion*/
END_TIMED_TEST
