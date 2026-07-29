#include "testutils.h"
#include "nestor.h"
#include "parser.h"
#include "compiler.h"
#include "aho_corasick.h"
#include "cache.h"
#include "utils.h"
#include "transport.h"
#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static void init() {
  /*#region*/
  test_init();
  system("mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin && gcc -O2 -shared -fPIC -Isrc/include tests/fixtures/test_dynamic_plugin.c -o ./plugins/test_dynamic_plugin.so");
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  /*#endregion*/
}

static void init_observability() {
  /*#region*/
  init();
  cr_redirect_stdout();
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

TestSuite(stage8, .init = init, .fini = fini);

TIMED_TEST(stage8, basic_aho_corasick_redaction, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // 1. Prepare dummy workflow context to get valid Jsonv_Arena
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Dummy\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {}\n"
    "}\n";
  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "token"), jsonv_val_str(allocate_jsonv_string(arena, "SECRET_TOKEN")));
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "password"), jsonv_val_str(allocate_jsonv_string(arena, "PA$$WORD")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "secrets"), jsonv_val_obj(secrets_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // 2. Build trie
  ACNode *root = ac_create_trie(arena, context_val);
  cr_assert_not_null(root);

  // 3. Test redact_stream
  const char *input = "Sending request with Authorization: Bearer SECRET_TOKEN to access database using PA$$WORD.";
  char output[256];
  size_t new_len = redact_stream(root, input, output, strlen(input));
  
  cr_assert_str_eq(output, "Sending request with Authorization: Bearer *** to access database using ***.");
  cr_assert_eq(new_len, strlen("Sending request with Authorization: Bearer *** to access database using ***."));

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, observability_log_redaction, init_observability, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // 1. Prepare dummy workflow context to get valid Jsonv_Arena
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Dummy\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {}\n"
    "}\n";
  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "token"), jsonv_val_str(allocate_jsonv_string(arena, "SECRET_TOKEN")));
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "pass"), jsonv_val_str(allocate_jsonv_string(arena, "PA$$WORD")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "secrets"), jsonv_val_obj(secrets_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // 2. Set globally
  ACNode *root = ac_create_trie(arena, context_val);
  set_global_ac_root(root);

  // 3. Log a message containing secrets
  event_log(Cyan, "Accessing database using PA$$WORD and token SECRET_TOKEN");

  // 4. Assert stdout has been redacted
  fflush(stdout);
  FILE *f = cr_get_redirected_stdout();
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  fgets(buf, sizeof(buf) - 1, f);
  
  cr_assert(strstr(buf, "Accessing database using *** and token ***"));
  cr_assert(strstr(buf, "PA$$WORD") == NULL);
  cr_assert(strstr(buf, "SECRET_TOKEN") == NULL);

  set_global_ac_root(NULL);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, async_plugin_timeout, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // We write a slow plugin script that sleeps 2s, but set a timeout of 100ms
  system("echo '#!/bin/sh' > ./plugins/slow_plugin");
  system("echo 'sleep 2' >> ./plugins/slow_plugin");
  system("chmod +x ./plugins/slow_plugin");

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Timeout Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"timeout_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"slow_step\",\n"
    "          \"uses\": \"./plugins/slow_plugin\",\n"
    "          \"timeout\": \"100ms\"\n"
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
  cr_assert_eq(run_status, ERR_HTTP_TRANSPORT);

  // Clean up
  unlink("./plugins/slow_plugin");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, concurrency_throttling, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Define a workflow with 4 parallel jobs, but set concurrency to 2
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Concurrency Throttling Test\",\n"
    "  \"concurrency\": 2,\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"job1\": {\n"
    "      \"type\": \"task\",\n"
    "      \"start\": true,\n"
    "      \"steps\": [ { \"id\": \"s1\", \"http\": { \"method\": \"POST\", \"url\": \"http://127.0.0.1:8080/delay/0.5\" } } ]\n"
    "    },\n"
    "    \"job2\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s2\", \"http\": { \"method\": \"POST\", \"url\": \"http://127.0.0.1:8080/delay/0.5\" } } ]\n"
    "    },\n"
    "    \"job3\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s3\", \"http\": { \"method\": \"POST\", \"url\": \"http://127.0.0.1:8080/delay/0.5\" } } ]\n"
    "    },\n"
    "    \"job4\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s4\", \"http\": { \"method\": \"POST\", \"url\": \"http://127.0.0.1:8080/delay/0.5\" } } ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(ast.max_concurrency, 2);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // Add mock response and initialize mock transport
  transport_mock_add_response("http://127.0.0.1:8080/delay/0.5", "POST", 200, "{\"status\": \"ok\"}");
  Transport *mock_trans = transport_mock_new(arena);
  cr_assert_not_null(mock_trans);

  struct timeval start, end;
  gettimeofday(&start, NULL);

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);

  gettimeofday(&end, NULL);
  double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
  
  // With concurrency=2, total time should be at least 2 * 0.5s = 1.0s.
  // With concurrency=4 (unlimited), total time would be ~0.5s.
  cr_assert(elapsed > 0.8, "Expected elapsed time to be > 0.8s due to throttling, got %f", elapsed);

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, http_retry_backoff, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // We write a workflow calling the /error endpoint on mock server (which returns 500)
  // retry_attempts: 2, retry_delay: 50ms, retry_backoff: linear
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Retry Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"retry_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"error_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/error\"\n"
    "          },\n"
    "          \"retry_attempts\": 2,\n"
    "          \"retry_delay\": \"50ms\",\n"
    "          \"retry_backoff\": \"linear\"\n"
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

  // Add mock response and initialize mock transport
  transport_mock_add_response("http://127.0.0.1:8080/error", "POST", 500, "{\"error\": \"failed\"}");
  Transport *mock_trans = transport_mock_new(arena);
  cr_assert_not_null(mock_trans);

  struct timeval start, end;
  gettimeofday(&start, NULL);

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_HTTP_TRANSPORT);

  gettimeofday(&end, NULL);
  double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;

  // Expected delay:
  // attempt 1: 50ms
  // attempt 2: 100ms
  // total delay should be around 150ms. So elapsed should be >= 0.13s.
  cr_assert(elapsed >= 0.13, "Expected elapsed time to be >= 0.13s due to linear retry delays, got %f", elapsed);

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, loop_memory_compaction, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // We define a loop that iterates 3 times
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Loop Compaction Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"loop_job\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"for_each\",\n"
    "      \"items\": \"[1, 2, 3]\",\n"
    "      \"max_iterations\": 10,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step1\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/delay/0.05\"\n"
    "          }\n"
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

  // Add mock response and initialize mock transport
  transport_mock_add_response("http://127.0.0.1:8080/delay/0.05", "POST", 200, "{\"status\": \"ok\"}");
  Transport *mock_trans = transport_mock_new(arena);
  cr_assert_not_null(mock_trans);

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, plugin_stderr_logging_and_redaction, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // We write a slow plugin script that sleeps 2s, but set a timeout of 100ms
  system("echo '#!/bin/sh' > ./plugins/stderr_plugin");
  system("echo 'echo \"{\\\"status\\\": \\\"ok\\\"}\"' >> ./plugins/stderr_plugin");
  system("echo 'echo \"Warning: secret PASSWORD_FOUND inside database logs!\" >&2' >> ./plugins/stderr_plugin");
  system("chmod +x ./plugins/stderr_plugin");

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Stderr Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"stderr_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"stderr_step\",\n"
    "          \"uses\": \"./plugins/stderr_plugin\"\n"
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

  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "pass"), jsonv_val_str(allocate_jsonv_string(arena, "PASSWORD_FOUND")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "secrets"), jsonv_val_obj(secrets_obj));

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // Assert redirection
  cr_redirect_stderr();

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Retrieve stderr output to verify the real-time logging redirected it
  fflush(stderr);
  FILE *f = cr_get_redirected_stderr();
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  bool found_prefix = false;
  while (fgets(buf, sizeof(buf) - 1, f)) {
    if (strstr(buf, "[stderr_step:stderr] Warning: secret *** inside database logs!")) {
      found_prefix = true;
      break;
    }
  }
  cr_assert(found_prefix, "Expected to find real-time stderr logs with prefix and secret redacted!");

  // Verify outcomes contains body and stderr
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "stderr_step", &step_outcome));
  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "body", &body_val));
  cr_assert_eq(body_val.tag, JSONV_VAL_OBJ); // parsed successfully since stderr did not pollute it!

  Jsonv_Value stderr_outcome;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "stderr", &stderr_outcome));
  cr_assert_eq(stderr_outcome.tag, JSONV_VAL_STRING);
  
  // Verify it contains redacted string
  StringView stderr_sv = { (const char *)stderr_outcome.as.p, jsonv_val_str_len(stderr_outcome) };
  cr_assert(sv_equals_cstr(stderr_sv, "Warning: secret *** inside database logs!\n"));

  // Clean up
  unlink("./plugins/stderr_plugin");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, plugin_ipc_socket_queries, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // We write the Python plugin to interact over Unix socket IPC
  FILE *f = fopen("./plugins/ipc_plugin", "w");
  cr_assert_not_null(f);
  fprintf(f, "#!/usr/bin/env python3\n");
  fprintf(f, "import socket, os, json\n");
  fprintf(f, "sock_path = os.environ.get(\"NESTOR_SOCKET\")\n");
  fprintf(f, "s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n");
  fprintf(f, "s.connect(sock_path)\n");
  fprintf(f, "req1 = json.dumps({\"action\": \"get\", \"path\": \"variables.test_var\"}) + \"\\n\"\n");
  fprintf(f, "s.sendall(req1.encode())\n");
  fprintf(f, "resp1 = s.recv(1024).decode()\n");
  fprintf(f, "data1 = json.loads(resp1.strip())\n");
  fprintf(f, "s2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)\n");
  fprintf(f, "s2.connect(sock_path)\n");
  fprintf(f, "req2 = json.dumps({\"action\": \"set\", \"path\": \"variables.plugin_updated\", \"value\": \"yes_it_works\"}) + \"\\n\"\n");
  fprintf(f, "s2.sendall(req2.encode())\n");
  fprintf(f, "resp2 = s2.recv(1024).decode()\n");
  fprintf(f, "print(json.dumps({\"received_val\": data1[\"value\"]}))\n");
  fclose(f);
  system("chmod +x ./plugins/ipc_plugin");


  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"IPC Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"ipc_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"ipc_step\",\n"
    "          \"uses\": \"./plugins/ipc_plugin\"\n"
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

  Jsonv_Obj *vars_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, vars_obj, allocate_jsonv_string(arena, "test_var"), jsonv_val_str(allocate_jsonv_string(arena, "hello_from_nestor")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "variables"), jsonv_val_obj(vars_obj));

  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Assert step outcome contains received_val
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "ipc_step", &step_outcome));
  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "body", &body_val));
  cr_assert_eq(body_val.tag, JSONV_VAL_OBJ);
  Jsonv_Value rec_val;
  cr_assert(jsonv_obj_get(body_val.as.p, "received_val", &rec_val));
  cr_assert_eq(rec_val.tag, JSONV_VAL_STRING);
  StringView rec_sv = { (const char *)rec_val.as.p, jsonv_val_str_len(rec_val) };
  cr_assert(sv_equals_cstr(rec_sv, "hello_from_nestor"));

  // Assert context contains variables.plugin_updated
  Jsonv_Value out_vars;
  cr_assert(jsonv_obj_get(context_val.as.p, "variables", &out_vars));
  Jsonv_Value updated_val;
  cr_assert(jsonv_obj_get(out_vars.as.p, "plugin_updated", &updated_val));
  cr_assert_eq(updated_val.tag, JSONV_VAL_STRING);
  StringView updated_sv = { (const char *)updated_val.as.p, jsonv_val_str_len(updated_val) };
  cr_assert(sv_equals_cstr(updated_sv, "yes_it_works"));

  // Clean up
  unlink("./plugins/ipc_plugin");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, mock_transport_seam, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Add mock response to the static mock transport list
  transport_mock_add_response("http://127.0.0.1:8080/mock-endpoint", "POST", 200, "{\"success\": true, \"message\": \"hello from mock\"}");

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Mock Transport Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"mock_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"mock_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/mock-endpoint\",\n"
    "            \"body\": { \"ping\": \"pong\" }\n"
    "          }\n"
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

  // Instantiate mock transport
  Transport *mock_trans = transport_mock_new(arena);
  cr_assert_not_null(mock_trans);

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Assert step outcome matches mocked body and status
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "mock_step", &step_outcome));
  
  Jsonv_Value status_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "status_code", &status_val));
  cr_assert_eq(status_val.as.i, 200);

  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "body", &body_val));
  cr_assert_eq(body_val.tag, JSONV_VAL_OBJ);
  
  Jsonv_Value msg_val;
  cr_assert(jsonv_obj_get(body_val.as.p, "message", &msg_val));
  StringView msg_sv = { (const char *)msg_val.as.p, jsonv_val_str_len(msg_val) };
  cr_assert(sv_equals_cstr(msg_sv, "hello from mock"));

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, conditional_execution_edge_cases, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Job A fails, Job B depends on A on failure (so B runs), Job C depends on A on success (so C is skipped).
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Conditional Edge Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"jobA\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"stepA\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/fail\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"jobB\": {\n"
    "      \"type\": \"task\",\n"
    "      \"depends_on\": [\n"
    "        {\n"
    "          \"job\": \"jobA\",\n"
    "          \"conditions\": [\"onFailure\"]\n"
    "        }\n"
    "      ],\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"stepB\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/success\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"jobC\": {\n"
    "      \"type\": \"task\",\n"
    "      \"depends_on\": [\n"
    "        {\n"
    "          \"job\": \"jobA\",\n"
    "          \"conditions\": [\"onSuccess\"]\n"
    "        }\n"
    "      ],\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"stepC\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/success\"\n"
    "          }\n"
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

  transport_mock_add_response("http://127.0.0.1:8080/fail", "POST", 500, "{\"error\": true}");
  transport_mock_add_response("http://127.0.0.1:8080/success", "POST", 200, "{\"success\": true}");
  Transport *mock_trans = transport_mock_new(arena);

  // Since jobA fails but jobB has depends_on_conditions for onFailure, jobB runs and succeeds.
  // jobC depends on jobA on onSuccess, so jobC is skipped.
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  (void)run_status;

  JobNode *j = ast.jobs_head;
  bool foundA = false, foundB = false, foundC = false;
  while (j) {
    if (sv_equals_cstr(j->id, "jobA")) {
      cr_assert_eq(j->execution_state, STATE_FAILED);
      foundA = true;
    } else if (sv_equals_cstr(j->id, "jobB")) {
      cr_assert_eq(j->execution_state, STATE_SUCCEEDED);
      foundB = true;
    } else if (sv_equals_cstr(j->id, "jobC")) {
      cr_assert_eq(j->execution_state, STATE_SKIPPED);
      foundC = true;
    }
    j = j->next_sorted;
  }
  cr_assert(foundA && foundB && foundC);

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, native_transform_job, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Transform Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"job1\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step1\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/data\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"transform_job\": {\n"
    "      \"type\": \"transform\",\n"
    "      \"depends_on\": [\"job1\"],\n"
    "      \"spec\": {\n"
    "        \"expression\": \"${{ jobs.job1.steps.step1.body.sensors[status='active'].name }}\"\n"
    "      }\n"
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

  transport_mock_add_response("http://127.0.0.1:8080/data", "POST", 200, 
    "{\"sensors\": [{\"name\": \"s1\", \"status\": \"active\"}, {\"name\": \"s2\", \"status\": \"inactive\"}]}");
  Transport *mock_trans = transport_mock_new(arena);

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Assert transform_job outcome contains the transformed data
  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "jobs", &jobs_val));
  Jsonv_Value trans_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "transform_job", &trans_outcome));
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(trans_outcome.as.p, "outputs", &outputs_val));
  
  cr_assert_eq(outputs_val.tag, JSONV_VAL_STRING);
  StringView name_sv = { outputs_val.as.p, jsonv_val_str_len(outputs_val) };
  cr_assert(sv_equals_cstr(name_sv, "s1"));

  mock_trans->ops->destroy(mock_trans);
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, cache_hit_and_ttl_eviction, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Initialize Cache
  unlink(".test_cache_hit.db");
  int32_t rc = cache_init(".test_cache_hit.db", 10);
  cr_assert_eq(rc, ERR_SUCCESS);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Cache Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"cache_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"cache_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/cache_url\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // First execution: cache miss, mock transport returns result
  transport_mock_clear();
  transport_mock_add_response("http://127.0.0.1:8080/cache_url", "POST", 200, "{\"val\": 42}");
  transport_mock_add_header("http://127.0.0.1:8080/cache_url", "POST", "Cache-Control", "max-age=3600");

  Transport *mock_trans = transport_mock_new(arena);
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  // Retrieve outcome and verify
  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "jobs", &jobs_val));
  Jsonv_Value cache_job_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "cache_job", &cache_job_outcome));
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(cache_job_outcome.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "cache_step", &step_outcome));
  Jsonv_Value body_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "body", &body_val));
  cr_assert_eq(body_val.tag, JSONV_VAL_OBJ);
  Jsonv_Value val_prop;
  cr_assert(jsonv_obj_get(body_val.as.p, "val", &val_prop));
  cr_assert_eq(val_prop.as.i, 42);

  // Reset AST and Context for second execution
  ast.jobs_head->execution_state = STATE_PENDING;
  ast.jobs_head->dependency_count = 0;
  Jsonv_Obj *root_obj2 = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj2, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL)));
  jsonv_obj_set(jsonv_arena, root_obj2, allocate_jsonv_string(arena, "env"), jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL)));
  Jsonv_Value context_val2 = jsonv_val_obj(root_obj2);

  // Second execution: cache hit! Clear mock transport
  transport_mock_clear();
  Transport *mock_trans2 = transport_mock_new(arena);
  run_status = run_workflow_opt(arena, &ast, &context_val2, mock_trans2);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans2->ops->destroy(mock_trans2);

  // Verify same value retrieved from cache
  Jsonv_Value jobs_val2;
  cr_assert(jsonv_obj_get(context_val2.as.p, "jobs", &jobs_val2));
  Jsonv_Value cache_job_outcome2;
  cr_assert(jsonv_obj_get(jobs_val2.as.p, "cache_job", &cache_job_outcome2));
  Jsonv_Value steps_val2;
  cr_assert(jsonv_obj_get(cache_job_outcome2.as.p, "steps", &steps_val2));
  Jsonv_Value step_outcome2;
  cr_assert(jsonv_obj_get(steps_val2.as.p, "cache_step", &step_outcome2));
  Jsonv_Value body_val2;
  cr_assert(jsonv_obj_get(step_outcome2.as.p, "body", &body_val2));
  cr_assert_eq(body_val2.tag, JSONV_VAL_OBJ);
  Jsonv_Value val_prop2;
  cr_assert(jsonv_obj_get(body_val2.as.p, "val", &val_prop2));
  cr_assert_eq(val_prop2.as.i, 42);

  // Clean up
  cache_close();
  unlink(".test_cache_hit.db");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, cache_304_validation, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Initialize Cache
  unlink(".test_cache_304.db");
  int32_t rc = cache_init(".test_cache_304.db", 10);
  cr_assert_eq(rc, ERR_SUCCESS);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Cache 304 Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"cache_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"cache_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/cache_url\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // First execution: Cache-Control: no-cache, ETag: "tag-123"
  transport_mock_clear();
  transport_mock_add_response("http://127.0.0.1:8080/cache_url", "POST", 200, "{\"val\": 100}");
  transport_mock_add_header("http://127.0.0.1:8080/cache_url", "POST", "Cache-Control", "no-cache");
  transport_mock_add_header("http://127.0.0.1:8080/cache_url", "POST", "ETag", "tag-123");

  Transport *mock_trans = transport_mock_new(arena);
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  // Reset AST and Context for second execution
  ast.jobs_head->execution_state = STATE_PENDING;
  ast.jobs_head->dependency_count = 0;
  Jsonv_Obj *root_obj2 = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj2, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL)));
  jsonv_obj_set(jsonv_arena, root_obj2, allocate_jsonv_string(arena, "env"), jsonv_val_obj(jsonv_obj_new(jsonv_arena, NULL)));
  Jsonv_Value context_val2 = jsonv_val_obj(root_obj2);

  // Second execution: stale entry, validate with ETag, server returns 304
  transport_mock_clear();
  transport_mock_add_response("http://127.0.0.1:8080/cache_url", "POST", 304, "");

  Transport *mock_trans2 = transport_mock_new(arena);
  run_status = run_workflow_opt(arena, &ast, &context_val2, mock_trans2);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans2->ops->destroy(mock_trans2);

  // Verify cached body was restored successfully
  Jsonv_Value jobs_val2;
  cr_assert(jsonv_obj_get(context_val2.as.p, "jobs", &jobs_val2));
  Jsonv_Value cache_job_outcome2;
  cr_assert(jsonv_obj_get(jobs_val2.as.p, "cache_job", &cache_job_outcome2));
  Jsonv_Value steps_val2;
  cr_assert(jsonv_obj_get(cache_job_outcome2.as.p, "steps", &steps_val2));
  Jsonv_Value step_outcome2;
  cr_assert(jsonv_obj_get(steps_val2.as.p, "cache_step", &step_outcome2));
  Jsonv_Value body_val2;
  cr_assert(jsonv_obj_get(step_outcome2.as.p, "body", &body_val2));
  cr_assert_eq(body_val2.tag, JSONV_VAL_OBJ);
  Jsonv_Value val_prop2;
  cr_assert(jsonv_obj_get(body_val2.as.p, "val", &val_prop2));
  cr_assert_eq(val_prop2.as.i, 100);

  // Clean up
  cache_close();
  unlink(".test_cache_304.db");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, dynamic_plugin_trusted_in_process, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Dynamic Plugin Trusted Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"plugin_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"run_plugin\",\n"
    "          \"uses\": \"./plugins/test_dynamic_plugin.so\",\n"
    "          \"sandboxed\": false,\n"
    "          \"with\": {\n"
    "            \"dummy\": \"unused\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "param_in"), jsonv_val_str(allocate_jsonv_string(arena, "hello_world")));
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  Transport *mock_trans = transport_mock_new(arena);
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  // Assert step outcome matches mocked body and status
  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "jobs", &jobs_val));
  Jsonv_Value job_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "plugin_job", &job_outcome));
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(job_outcome.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "run_plugin", &step_outcome));

  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "outputs", &outputs_val));
  cr_assert_eq(outputs_val.tag, JSONV_VAL_OBJ);

  Jsonv_Value status_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "status", &status_prop));
  cr_assert_eq(status_prop.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){ status_prop.as.p, jsonv_val_str_len(status_prop) }, "success"));

  Jsonv_Value val_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "computed_val", &val_prop));
  cr_assert_eq(val_prop.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){ val_prop.as.p, jsonv_val_str_len(val_prop) }, "Processed: hello_world"));

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, dynamic_plugin_sandboxed_subprocess, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Dynamic Plugin Sandboxed Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"plugin_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"run_plugin\",\n"
    "          \"uses\": \"./plugins/test_dynamic_plugin.so\",\n"
    "          \"sandboxed\": true,\n"
    "          \"with\": {\n"
    "            \"dummy\": \"unused\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t parse_status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(parse_status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_SUCCESS);

  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "param_in"), jsonv_val_str(allocate_jsonv_string(arena, "hello_sandbox")));
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));
  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  Transport *mock_trans = transport_mock_new(arena);
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  // Assert step outcome matches mocked body and status
  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "jobs", &jobs_val));
  Jsonv_Value job_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "plugin_job", &job_outcome));
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(job_outcome.as.p, "steps", &steps_val));
  Jsonv_Value step_outcome;
  cr_assert(jsonv_obj_get(steps_val.as.p, "run_plugin", &step_outcome));

  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(step_outcome.as.p, "outputs", &outputs_val));
  cr_assert_eq(outputs_val.tag, JSONV_VAL_OBJ);

  Jsonv_Value status_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "status", &status_prop));
  cr_assert_eq(status_prop.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){ status_prop.as.p, jsonv_val_str_len(status_prop) }, "success"));

  Jsonv_Value val_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "computed_val", &val_prop));
  cr_assert_eq(val_prop.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){ val_prop.as.p, jsonv_val_str_len(val_prop) }, "Processed: hello_sandbox"));

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, deterministic_boundaries_multiple_starts, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // 2 root jobs without start flag
  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Multiple Starts Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"job1\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s1\", \"uses\": \"dummy_plugin\" } ]\n"
    "    },\n"
    "    \"job2\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s2\", \"uses\": \"dummy_plugin\" } ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_INVALID_BOUNDARY);

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, deterministic_boundaries_unjoined_fork, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Unjoined Fork Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"start_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"start\": true,\n"
    "      \"steps\": [ { \"id\": \"s0\", \"uses\": \"dummy_plugin\" } ]\n"
    "    },\n"
    "    \"fork_job\": {\n"
    "      \"type\": \"fork\",\n"
    "      \"depends_on\": \"start_job\",\n"
    "      \"branches\": [\"job_a\", \"job_b\"]\n"
    "    },\n"
    "    \"job_a\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [ { \"id\": \"s1\", \"uses\": \"dummy_plugin\" } ]\n"
    "    },\n"
    "    \"job_b\": {\n"
    "      \"type\": \"task\",\n"
    "      \"end\": true,\n"
    "      \"steps\": [ { \"id\": \"s2\", \"uses\": \"dummy_plugin\" } ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  int32_t compile_status = compile_workflow(arena, &ast);
  cr_assert_eq(compile_status, ERR_INVALID_BOUNDARY);

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, deterministic_boundaries_valid_flow, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Valid Flow Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"job1\": {\n"
    "      \"type\": \"transform\",\n"
    "      \"start\": true,\n"
    "      \"spec\": { \"expression\": \"10 + 20\" },\n"
    "      \"return\": {\n"
    "        \"result_val\": \"${{ jobs.job1.result }}\",\n"
    "        \"status_str\": \"completed\"\n"
    "      }\n"
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

  Transport *mock_trans = transport_mock_new(arena);
  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  // Verify that outputs exists under top-level outputs
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "outputs", &outputs_val));
  cr_assert_eq(outputs_val.tag, JSONV_VAL_OBJ);

  Jsonv_Value res_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "result_val", &res_prop));
  cr_assert_eq(res_prop.tag, JSONV_VAL_INT);
  cr_assert_eq(res_prop.as.i, 30);

  Jsonv_Value status_prop;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "status_str", &status_prop));
  cr_assert_eq(status_prop.tag, JSONV_VAL_STRING);
  cr_assert(sv_equals_cstr((StringView){ status_prop.as.p, jsonv_val_str_len(status_prop) }, "completed"));

  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage8, stream_parsing_and_chunk_loop, init, fini)
  /*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  const char *yaml =
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Stream Chunk Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"fetch_data\": {\n"
    "      \"type\": \"task\",\n"
    "      \"start\": true,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"download\",\n"
    "          \"http\": {\n"
    "            \"method\": \"GET\",\n"
    "            \"url\": \"http://127.0.0.1:8080/api/stream_test\",\n"
    "            \"stream\": true\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"process_chunks\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"stream_chunk\",\n"
    "      \"source\": \"jobs.fetch_data.steps.download.stream\",\n"
    "      \"items\": \"data.id\",\n"
    "      \"chunk_record_limit\": 2,\n"
    "      \"depends_on\": [\"fetch_data\"],\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"mock_call\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/api/mock_chunk_step\",\n"
    "            \"body\": {\n"
    "              \"first_id\": \"${{ chunk[0].id }}\"\n"
    "            }\n"
    "          }\n"
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

  Transport *mock_trans = transport_mock_new(arena);
  transport_mock_add_response("http://127.0.0.1:8080/api/stream_test", "GET", 200, "{\"data\": [{\"id\": 10}, {\"id\": 20}, {\"id\": 30}]}");
  transport_mock_add_response("http://127.0.0.1:8080/api/mock_chunk_step", "POST", 200, "{\"status\": \"ok\"}");

  int32_t run_status = run_workflow_opt(arena, &ast, &context_val, mock_trans);
  cr_assert_eq(run_status, ERR_SUCCESS);
  mock_trans->ops->destroy(mock_trans);

  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(context_val.as.p, "jobs", &jobs_val));
  cr_assert_eq(jobs_val.tag, JSONV_VAL_OBJ);

  Jsonv_Value loop_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "process_chunks", &loop_outcome));
  cr_assert_eq(loop_outcome.tag, JSONV_VAL_OBJ);

  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(loop_outcome.as.p, "steps", &steps_val));
  cr_assert_eq(steps_val.tag, JSONV_VAL_OBJ);

  Jsonv_Value mock_call_val;
  cr_assert(jsonv_obj_get(steps_val.as.p, "mock_call", &mock_call_val));
  cr_assert_eq(mock_call_val.tag, JSONV_VAL_ARRAY);
  cr_assert_eq(jsonv_arr_length(mock_call_val.as.p), 2);

  Jsonv_Value iter_0;
  cr_assert(jsonv_arr_get(mock_call_val.as.p, 0, &iter_0));
  cr_assert_eq(iter_0.tag, JSONV_VAL_OBJ);

  Jsonv_Value sc_prop;
  cr_assert(jsonv_obj_get(iter_0.as.p, "status_code", &sc_prop));
  cr_assert_eq(sc_prop.tag, JSONV_VAL_INT);
  cr_assert_eq(sc_prop.as.i, 200);

  unlink(".nestor_stream_download.json");
  arena_destroy(arena);
  /*#endregion*/
END_TIMED_TEST

