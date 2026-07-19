#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>
#include "testutils.h"
#include "arena.h"
#include "error_codes.h"
#include <criterion/criterion.h>

static void *my_jsonv_arena_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void my_jsonv_arena_reset(void *user_data) {
  /*#region*/
  arena_reset((Arena *)user_data);
  /*#endregion*/
}

static void my_jsonv_arena_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  arena_restore((Arena *)user_data, keep_size);
  /*#endregion*/
}

static void my_jsonv_arena_destroy(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static const Jsonv_Arena_Ops my_jsonv_ops = {
  .alloc = my_jsonv_arena_alloc,
  .reset = my_jsonv_arena_reset,
  .reset_to = my_jsonv_arena_reset_to,
  .destroy = my_jsonv_arena_destroy
};

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

static const char *workflow_schema_json = 
  "{\n"
  "  \"type\": \"object\",\n"
  "  \"required\": [\"version\", \"name\", \"on\", \"jobs\"],\n"
  "  \"properties\": {\n"
  "    \"version\": { \"type\": \"string\" },\n"
  "    \"name\": { \"type\": \"string\", \"maxLength\": 128 },\n"
  "    \"on\": {\n"
  "      \"type\": \"object\",\n"
  "      \"minProperties\": 1\n"
  "    },\n"
  "    \"concurrency\": { \"type\": \"integer\" },\n"
  "    \"env\": {\n"
  "      \"type\": \"object\",\n"
  "      \"additionalProperties\": { \"type\": \"string\" }\n"
  "    },\n"
  "    \"jobs\": {\n"
  "      \"type\": \"object\",\n"
  "      \"additionalProperties\": {\n"
  "        \"type\": \"object\",\n"
  "        \"properties\": {\n"
  "          \"type\": { \"type\": \"string\", \"enum\": [\"task\", \"if\", \"switch\", \"fork\", \"join\", \"loop\", \"wait_signal\", \"wait_timer\"] },\n"
  "          \"name\": { \"type\": \"string\" },\n"
  "          \"depends_on\": {\n"
  "            \"anyOf\": [\n"
  "              { \"type\": \"string\" },\n"
  "              { \"type\": \"array\", \"items\": { \"type\": \"string\" } }\n"
  "            ]\n"
  "          },\n"
  "          \"steps\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": {\n"
  "              \"type\": \"object\",\n"
  "              \"properties\": {\n"
  "                \"id\": { \"type\": \"string\" },\n"
  "                \"http\": {\n"
  "                  \"type\": \"object\",\n"
  "                  \"required\": [\"method\", \"url\"],\n"
  "                  \"properties\": {\n"
  "                    \"method\": { \"type\": \"string\", \"enum\": [\"GET\", \"POST\", \"PUT\", \"PATCH\", \"DELETE\"] },\n"
  "                    \"url\": { \"type\": \"string\" },\n"
  "                    \"headers\": { \"type\": \"object\" },\n"
  "                    \"body\": { \"type\": \"object\" },\n"
  "                    \"timeout\": { \"type\": \"string\" },\n"
  "                    \"mtls_profile\": { \"type\": \"string\" }\n"
  "                  }\n"
  "                },\n"
  "                \"uses\": { \"type\": \"string\" },\n"
  "                \"with\": { \"type\": \"object\" },\n"
  "                \"timeout\": { \"type\": \"string\" },\n"
  "                \"retry_attempts\": { \"type\": \"integer\" },\n"
  "                \"retry_backoff\": { \"type\": \"string\" },\n"
  "                \"retry_delay\": { \"type\": \"string\" }\n"
  "              }\n"
  "            }\n"
  "          },\n"
  "          \"condition\": { \"type\": \"string\" },\n"
  "          \"then\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"else\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"cases\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": {\n"
  "              \"type\": \"object\",\n"
  "              \"required\": [\"condition\", \"then\"],\n"
  "              \"properties\": {\n"
  "                \"condition\": { \"type\": \"string\" },\n"
  "                \"then\": {\n"
  "                  \"type\": \"array\",\n"
  "                  \"items\": { \"type\": \"string\" }\n"
  "                }\n"
  "              }\n"
  "            }\n"
  "          },\n"
  "          \"default\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"branches\": {\n"
  "            \"type\": \"array\",\n"
  "            \"items\": { \"type\": \"string\" }\n"
  "          },\n"
  "          \"strategy\": { \"type\": \"string\" },\n"
  "          \"n_required\": { \"type\": \"integer\" },\n"
  "          \"loop_type\": { \"type\": \"string\", \"enum\": [\"while\", \"for_each\"] },\n"
  "          \"max_iterations\": { \"type\": \"integer\" },\n"
  "          \"items\": { \"type\": \"string\" },\n"
  "          \"correlation_id\": { \"type\": \"string\" },\n"
  "          \"duration\": { \"type\": \"string\" }\n"
  "        }\n"
  "      }\n"
  "    }\n"
  "  }\n"
  "}\n";

TIMED_TEST(stage2, jsonv_schema_validation_real_spec, init, fini)
/*#region*/
  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  Jsonv_Arena *jsonv_arena = jsonv_arena_new_custom(&my_jsonv_ops, arena);
  cr_assert_not_null(jsonv_arena);

  Jsonv_Config config = {0};
  config.default_block_size = 4096;
  config.max_limit = 10 * 1024 * 1024;
  config.max_depth = 100;
  config.max_values = 10000;
  config.max_objects = 1000;
  config.max_array = 1000;
  config.max_string_bytes = 1024 * 1024;

  Jsonv_Error err = {0};
  Jsonv_Schema *schema = jsonv_schema_compile(jsonv_arena, (const unsigned char *)workflow_schema_json, &config, &err);
  cr_assert_not_null(schema, "Failed to compile workflow schema: %s", err.description);

  // 1. Validate valid workflow HTTP
  const char *valid_http_json = 
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Basic HTTP Workflow\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"http_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"provision_database\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/api/provision\",\n"
    "            \"headers\": {\n"
    "              \"Content-Type\": \"application/json\"\n"
    "            }\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  {
    Jsonv_Arena_Error err_code = 0;
    Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
    cr_assert_not_null(ctx);
    bool parsed = jsonv_ctx_parse_data(ctx, (const unsigned char *)valid_http_json);
    cr_assert(parsed, "Parse failed: %s", jsonv_ctx_get_error(ctx) ? jsonv_ctx_get_error(ctx)->description : "unknown");
    cr_assert(jsonv_ctx_validate(ctx, schema));
  }

  // 2. Validate invalid workflow (missing required field)
  const char *invalid_missing_on_json = 
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Basic HTTP Workflow\",\n"
    "  \"jobs\": {}\n"
    "}\n";

  {
    Jsonv_Arena_Error err_code = 0;
    Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
    cr_assert_not_null(ctx);
    bool parsed = jsonv_ctx_parse_data(ctx, (const unsigned char *)invalid_missing_on_json);
    cr_assert(parsed, "Parse failed: %s", jsonv_ctx_get_error(ctx) ? jsonv_ctx_get_error(ctx)->description : "unknown");
    cr_assert(!jsonv_ctx_validate(ctx, schema));
  }

  // 3. Validate loop iteration example format
  const char *valid_loop_json = 
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Loop Iteration Example\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"while_loop\": {\n"
    "      \"type\": \"loop\",\n"
    "      \"loop_type\": \"while\",\n"
    "      \"condition\": \"${{ index < 3 }}\",\n"
    "      \"max_iterations\": 5,\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"delay_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:8080/delay/0.1\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  {
    Jsonv_Arena_Error err_code = 0;
    Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
    cr_assert_not_null(ctx);
    bool parsed = jsonv_ctx_parse_data(ctx, (const unsigned char *)valid_loop_json);
    cr_assert(parsed, "Parse failed: %s", jsonv_ctx_get_error(ctx) ? jsonv_ctx_get_error(ctx)->description : "unknown");
    cr_assert(jsonv_ctx_validate(ctx, schema));
  }

  // Validate an invalid payload (maxLength violated)
  {
    Jsonv_Arena_Error err_code = 0;
    Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, &config, &err_code);
    cr_assert_not_null(ctx);
    
    bool parsed = jsonv_ctx_parse_data(ctx, (const unsigned char *)"{\"version\": \"1.0\", \"name\": \"01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890\", \"on\": {\"manual\": {}}, \"jobs\": {}}");
    cr_assert(parsed);

    bool valid = jsonv_ctx_validate(ctx, schema);
    cr_assert(!valid, "Validation succeeded but should have failed");
    
    const Jsonv_Error *val_err = jsonv_ctx_get_error(ctx);
    cr_assert_not_null(val_err);
    cr_assert_eq(val_err->type, Jsonv_MaxLength_error);
  }

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
