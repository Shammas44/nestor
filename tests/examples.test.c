#include "testutils.h"
#include "nestor.h"
#include "parser.h"
#include "compiler.h"
#include "transport.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void init() {
  /*#region*/
  test_init();
  system("mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin");

  // Register mock HTTP responses matching endpoints hit in example files
  transport_mock_add_response("127.0.0.1:8080/api/provision", "POST", 200, "{\"success\": true, \"resource_id\": \"res-123\", \"status\": \"active\"}");
  transport_mock_add_response("127.0.0.1:8080/api/validate", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/api/deploy/", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/api/notify", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/delay/", "POST", 200, "{\"success\": true}");
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  unlink("./plugins/mock_plugin");
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

  // Setup mock execution context (Inputs, Secrets, Env)
  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  // 1. Inputs
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "user_id"), jsonv_val_str(allocate_jsonv_string(arena, "admin_user")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "tier"), jsonv_val_str(allocate_jsonv_string(arena, "enterprise")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "region"), jsonv_val_str(allocate_jsonv_string(arena, "eu")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "val"), jsonv_val_int(1));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "index"), jsonv_val_int(0));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  // 2. Secrets
  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "API_KEY"), jsonv_val_str(allocate_jsonv_string(arena, "my-super-secret-key")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "secrets"), jsonv_val_obj(secrets_obj));

  // 3. Environment variables
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, env_obj, allocate_jsonv_string(arena, "ENV_NAME"), jsonv_val_str(allocate_jsonv_string(arena, "production")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // Instantiate mock transport
  Transport *mock_trans = transport_mock_new(arena);
  cr_assert_not_null(mock_trans);

  // Run the execution engine
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);

  mock_trans->ops->destroy(mock_trans);
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
