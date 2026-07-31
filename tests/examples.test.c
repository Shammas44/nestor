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
  transport_mock_clear();

  // Register mock HTTP responses matching endpoints hit in example files
  transport_mock_add_response("127.0.0.1:8080/api/provision", "POST", 200, "{\"success\": true, \"resource_id\": \"res-123\", \"status\": \"active\"}");
  transport_mock_add_response("127.0.0.1:8080/api/validate", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/api/deploy/", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/api/notify", "POST", 200, "{\"success\": true}");
  transport_mock_add_response("127.0.0.1:8080/delay/", "POST", 200, "{\"success\": true}");

  // SWAPI Mock Responses
  transport_mock_add_response("swapi.dev/api/people/1/", "GET", 200, "{\"name\": \"Luke Skywalker\", \"homeworld\": \"https://swapi.dev/api/planets/1/\"}");
  transport_mock_add_response("swapi.dev/api/people/2/", "GET", 200, "{\"name\": \"C-3PO\", \"height\": 167}");
  transport_mock_add_response("swapi.dev/api/planets/1/", "GET", 200, "{\"name\": \"Tatooine\", \"population\": 200000, \"climate\": \"arid\", \"gravity\": \"1 standard\"}");
  transport_mock_add_response("swapi.dev/api/planets/2/", "GET", 200, "{\"name\": \"Alderaan\", \"population\": 2000000000, \"climate\": \"temperate\", \"gravity\": \"1 standard\"}");
  transport_mock_add_response("swapi.dev/api/starships/9/", "GET", 200, "{\"name\": \"Death Star\"}");
  transport_mock_add_response("swapi.dev/api/starships/10/", "GET", 200, "{\"name\": \"Millennium Falcon\", \"passengers\": \"6\"}");
  transport_mock_add_response("swapi.dev/api/starships/12/", "GET", 200, "{\"name\": \"X-wing\", \"passengers\": \"0\"}");
  transport_mock_add_response("swapi.dev/api/films/1/", "GET", 200, "{\"title\": \"A New Hope\", \"characters\": [\"https://swapi.dev/api/people/1/\", \"https://swapi.dev/api/people/2/\"]}");
  // Multi-Region Employee Mock Responses
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-east/auth/login", "POST", 200, "{\"status\": \"authenticated\", \"region\": \"us-east\", \"token\": \"Bearer token_us_east_123\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-east/employees?max_age=30", "GET", 200, "{\"region\": \"us-east\", \"employee_ids\": [\"emp_101\", \"emp_102\"]}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-east/employees/emp_101", "GET", 200, "{\"id\": \"emp_101\", \"name\": \"Alice Smith\", \"age\": 25, \"department\": \"Engineering\", \"salary\": 95000, \"region\": \"us-east\", \"internal_notes\": \"Top performer\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-east/employees/emp_102", "GET", 200, "{\"id\": \"emp_102\", \"name\": \"Bob Jones\", \"age\": 28, \"department\": \"Product\", \"salary\": 88000, \"region\": \"us-east\", \"internal_notes\": \"Remote\"}");

  transport_mock_add_response("127.0.0.1:8080/api/v1/us-west/auth/login", "POST", 200, "{\"status\": \"authenticated\", \"region\": \"us-west\", \"token\": \"Bearer token_us_west_123\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-west/employees?max_age=30", "GET", 200, "{\"region\": \"us-west\", \"employee_ids\": [\"emp_201\"]}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/us-west/employees/emp_201", "GET", 200, "{\"id\": \"emp_201\", \"name\": \"David Miller\", \"age\": 22, \"department\": \"Design\", \"salary\": 78000, \"region\": \"us-west\", \"internal_notes\": \"Intern\"}");

  transport_mock_add_response("127.0.0.1:8080/api/v1/eu-west/auth/login", "POST", 200, "{\"status\": \"authenticated\", \"region\": \"eu-west\", \"token\": \"Bearer token_eu_west_123\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/eu-west/employees?max_age=30", "GET", 200, "{\"region\": \"eu-west\", \"employee_ids\": [\"emp_301\"]}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/eu-west/employees/emp_301", "GET", 200, "{\"id\": \"emp_301\", \"name\": \"Fiona Garcia\", \"age\": 29, \"department\": \"Operations\", \"salary\": 82000, \"region\": \"eu-west\", \"internal_notes\": \"Paris\"}");

  transport_mock_add_response("127.0.0.1:8080/api/v1/ap-south/auth/login", "POST", 200, "{\"status\": \"authenticated\", \"region\": \"ap-south\", \"token\": \"Bearer token_ap_south_123\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/ap-south/employees?max_age=30", "GET", 200, "{\"region\": \"ap-south\", \"employee_ids\": [\"emp_401\", \"emp_402\"]}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/ap-south/employees/emp_401", "GET", 200, "{\"id\": \"emp_401\", \"name\": \"Hana Tanaka\", \"age\": 26, \"department\": \"Engineering\", \"salary\": 85000, \"region\": \"ap-south\", \"internal_notes\": \"Tokyo\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/ap-south/employees/emp_402", "GET", 200, "{\"id\": \"emp_402\", \"name\": \"Ian Chen\", \"age\": 24, \"department\": \"Marketing\", \"salary\": 72000, \"region\": \"ap-south\", \"internal_notes\": \"Singapore\"}");
  transport_mock_add_response("127.0.0.1:8080/api/v1/data/report", "GET", 200, "{\"status\": \"generated\", \"data\": [1,2,3]}");
  system("mkdir -p ./plugins && gcc -O2 -shared -fPIC -Isrc/include tests/fixtures/test_dynamic_plugin.c -o ./plugins/test_dynamic_plugin.so");
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

static Jsonv_Value get_step_outcome(Arena *arena, Jsonv_Obj *root_obj, const char *job_id, const char *step_id) {
  /*#region*/
  (void)arena;
  Jsonv_Value jobs_val;
  if (jsonv_obj_get(root_obj, "jobs", &jobs_val) && jobs_val.tag == JSONV_VAL_OBJ) {
    Jsonv_Value job_val;
    if (jsonv_obj_get(jobs_val.as.p, job_id, &job_val) && job_val.tag == JSONV_VAL_OBJ) {
      Jsonv_Value steps_val;
      if (jsonv_obj_get(job_val.as.p, "steps", &steps_val) && steps_val.tag == JSONV_VAL_OBJ) {
        Jsonv_Value step_val;
        if (jsonv_obj_get(steps_val.as.p, step_id, &step_val)) {
          return step_val;
        }
      }
    }
  }
  return jsonv_val_undefined();
  /*#endregion*/
}

static int64_t get_step_status_code(Arena *arena, Jsonv_Obj *root_obj, const char *job_id, const char *step_id) {
  /*#region*/
  Jsonv_Value outcome = get_step_outcome(arena, root_obj, job_id, step_id);
  if (outcome.tag == JSONV_VAL_ARRAY) {
    Jsonv_Value first_item;
    if (jsonv_arr_get(outcome.as.p, 0, &first_item)) {
      outcome = first_item;
    }
  }
  if (outcome.tag == JSONV_VAL_OBJ) {
    Jsonv_Value status_code_val;
    if (jsonv_obj_get(outcome.as.p, "status_code", &status_code_val) && status_code_val.tag == JSONV_VAL_INT) {
      return status_code_val.as.i;
    }
  }
  return -1;
  /*#endregion*/
}

static Jsonv_Value run_example_test(Arena *arena, const char *filepath) {
  /*#region*/
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
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "param_in"), jsonv_val_str(allocate_jsonv_string(arena, "e2e_showcase")));

  Jsonv_Arr *planet_arr = jsonv_arr_new(jsonv_arena);
  jsonv_arr_set(jsonv_arena, planet_arr, 0, jsonv_val_str(allocate_jsonv_string(arena, "1")));
  jsonv_arr_set(jsonv_arena, planet_arr, 1, jsonv_val_str(allocate_jsonv_string(arena, "2")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "planet_ids"), jsonv_val_arr(planet_arr));

  Jsonv_Arr *char_arr = jsonv_arr_new(jsonv_arena);
  jsonv_arr_set(jsonv_arena, char_arr, 0, jsonv_val_str(allocate_jsonv_string(arena, "1")));
  jsonv_arr_set(jsonv_arena, char_arr, 1, jsonv_val_str(allocate_jsonv_string(arena, "2")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "character_ids"), jsonv_val_arr(char_arr));

  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  // 2. Secrets
  Jsonv_Obj *secrets_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "API_KEY"), jsonv_val_str(allocate_jsonv_string(arena, "my-super-secret-key")));
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "SWAPI_KEY"), jsonv_val_str(allocate_jsonv_string(arena, "secret-swapi-key")));
  jsonv_obj_set(jsonv_arena, secrets_obj, allocate_jsonv_string(arena, "SWAPI_PROXY_KEY"), jsonv_val_str(allocate_jsonv_string(arena, "secret-proxy-key")));
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
  cr_assert_eq(run_status, ERR_SUCCESS, "run_status was %d", (int)run_status);

  mock_trans->ops->destroy(mock_trans);
  return context_val;
  /*#endregion*/
}

TIMED_TEST(examples, basic_http, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/01_basic_http.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "http_job", "provision_database");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, plugin_pipeline, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/02_plugin_pipeline.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "plugin_job", "run_integration");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, enterprise_deploy, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/03_enterprise_deploy.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "notify_success", "slack_notify");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, multistep_jsonata, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/04_multistep_jsonata.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "processing_pipeline", "transform_and_log");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, concurrency_resume, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/05_concurrency_resume.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "downstream_task", "notify_downstream");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, secrets_redaction, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/06_secrets_redaction.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "redact_job", "deploy_step");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, conditionals, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/07_conditionals.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "deploy_eu", "eu_post");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, loops, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/08_loops.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "for_each_loop", "deploy_region");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, multi_region_employees, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/09_multi_region_employees.yaml");
  int64_t status = get_step_status_code(arena, ctx.as.p, "consolidate_employees", "log_output");
  cr_assert_eq(status, 0, "Expected status 0, got %ld", (long)status);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

// SWAPI Examples
TIMED_TEST(examples, swapi_01_character_deep_dive, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/01_character_deep_dive.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "heavily_populated", "log_heavy");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_02_starship_fleet_concurrency, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/02_starship_fleet_concurrency.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "evaluate_fleet", "log_fleet_stats");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_03_planet_colonization_loop, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/03_planet_colonization_loop.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "planet_evaluation", "check_hospitable");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_04_film_character_association, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/04_film_character_association.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "verify_characters", "log_association");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_05_api_rate_limiter_retry, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/05_api_rate_limiter_retry.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "fetch_starship_resiliently", "get_starship");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_06_secret_authorized_proxy, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/06_secret_authorized_proxy.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "query_proxy", "get_proxied_character");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_07_observability_redaction, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/07_observability_redaction.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "redact_luke", "log_sensitive_output");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_08_conditional_species_branching, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/08_conditional_species_branching.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "short_character_branch", "log_short");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_09_async_wait_timer, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/09_async_wait_timer.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "launch_check", "confirm_launch");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_10_coordinated_multistage_orchestration, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/swapi/10_coordinated_multistage_orchestration.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "final_report", "compile_results");
  cr_assert_eq(status, 0);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, showcase_advanced_features, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  Jsonv_Value ctx = run_example_test(arena, "examples/10_showcase_advanced_features.json");
  int64_t status = get_step_status_code(arena, ctx.as.p, "aggregate_and_cache", "cache_revalidated_http");
  cr_assert_eq(status, 200);
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, bigdata_test, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  transport_mock_add_response("127.0.0.1:8080/api/bigdata/2.0", "GET", 200, "{\"size_mb\": 2.0, \"element_count\": 3, \"data\": [{\"id\": 10}, {\"id\": 20}, {\"id\": 30}]}");
  transport_mock_add_response("127.0.0.1:8080/api/transform", "POST", 200, "{\"status\": \"ok\", \"received_body\": {\"records\": [{\"id\": 10}, {\"id\": 20}, {\"id\": 30}]}}");

  Jsonv_Value ctx = run_example_test(arena, "examples/11_bigdata_test.yaml");
  int64_t status = get_step_status_code(arena, ctx.as.p, "fetch_bigdata", "get_payload");
  cr_assert_eq(status, 200);

  Jsonv_Value jobs_val;
  cr_assert(jsonv_obj_get(ctx.as.p, "jobs", &jobs_val));
  Jsonv_Value loop_outcome;
  cr_assert(jsonv_obj_get(jobs_val.as.p, "process_chunks", &loop_outcome));
  Jsonv_Value steps_val;
  cr_assert(jsonv_obj_get(loop_outcome.as.p, "steps", &steps_val));
  Jsonv_Value post_chunk_arr;
  cr_assert(jsonv_obj_get(steps_val.as.p, "post_chunk", &post_chunk_arr));
  cr_assert_eq(post_chunk_arr.tag, JSONV_VAL_ARRAY);
  cr_assert(jsonv_arr_length(post_chunk_arr.as.p) > 0);

  unlink(".nestor_stream_get_payload.json");
  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(examples, swapi_provider_showcase, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  transport_mock_add_response("swapi.info/api/people/1/", "GET", 200, "{\"name\": \"Luke Skywalker\", \"homeworld\": \"https://swapi.info/api/planets/1/\"}");
  transport_mock_add_response("swapi.info/api/starships/10/", "GET", 200, "{\"name\": \"Millennium Falcon\", \"passengers\": \"6\"}");

  Jsonv_Value ctx = run_example_test(arena, "examples/13_swapi_provider_showcase.yaml");

  // Check step execution success
  int64_t luke_status = get_step_status_code(arena, ctx.as.p, "fetch_luke", "get_luke");
  cr_assert_eq(luke_status, 200);
  int64_t ship_status = get_step_status_code(arena, ctx.as.p, "fetch_luke", "get_ship");
  cr_assert_eq(ship_status, 200);

  // Check return values
  Jsonv_Value outputs_val;
  cr_assert(jsonv_obj_get(ctx.as.p, "outputs", &outputs_val));

  Jsonv_Value person_val, ship_val, passengers_val;
  cr_assert(jsonv_obj_get(outputs_val.as.p, "person_name", &person_val));
  cr_assert(sv_equals_cstr((StringView){person_val.as.p, jsonv_val_str_len(person_val)}, "Luke Skywalker"));

  cr_assert(jsonv_obj_get(outputs_val.as.p, "ship_name", &ship_val));
  cr_assert(sv_equals_cstr((StringView){ship_val.as.p, jsonv_val_str_len(ship_val)}, "Millennium Falcon"));

  cr_assert(jsonv_obj_get(outputs_val.as.p, "passengers", &passengers_val));
  cr_assert(sv_equals_cstr((StringView){passengers_val.as.p, jsonv_val_str_len(passengers_val)}, "6"));

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

