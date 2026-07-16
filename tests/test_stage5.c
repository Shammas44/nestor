#include <criterion/criterion.h>
#include "nestor.h"
#include "parser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

static bool validate_json_string(Jsonv_Arena *jsonv_arena, const char *json_data, const char *schema_json) {
  /*#region*/
  Jsonv_Error err;
  Jsonv_Schema *schema = jsonv_schema_compile(jsonv_arena, (const unsigned char *)schema_json, NULL, &err);
  if (!schema) {
    fprintf(stderr, "Schema Compilation Error: %s at path %s\n", err.description, err.path);
    return false;
  }
  
  Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
  if (!ctx) return false;
  
  if (!jsonv_ctx_parse_data(ctx, (const unsigned char *)json_data)) {
    const Jsonv_Error *parse_err = jsonv_ctx_get_error(ctx);
    fprintf(stderr, "JSON Parse Error: %s at path %s\n", parse_err->description, parse_err->path);
    return false;
  }
  
  bool ok = jsonv_ctx_validate(ctx, schema);
  if (!ok) {
    const Jsonv_Error *val_err = jsonv_ctx_get_error(ctx);
    fprintf(stderr, "JSON Schema Validation Error: %s at path %s (type %d)\n", val_err->description, val_err->path, val_err->type);
  }
  
  return ok;
  /*#endregion*/
}

static void run_mock_http_server(int write_fd) {
  /*#region*/
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket failed");
    exit(1);
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr("127.0.0.1");
  address.sin_port = htons(0); // Bind to any free port

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(1);
  }

  if (listen(server_fd, 1) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(1);
  }

  // Get the assigned port
  struct sockaddr_in bound_addr;
  socklen_t addr_len = sizeof(bound_addr);
  if (getsockname(server_fd, (struct sockaddr *)&bound_addr, &addr_len) < 0) {
    perror("getsockname failed");
    close(server_fd);
    exit(1);
  }
  int port = ntohs(bound_addr.sin_port);

  // Write port to pipe
  write(write_fd, &port, sizeof(port));
  close(write_fd);

  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd >= 0) {
    char buf[1024];
    read(client_fd, buf, sizeof(buf)); // Read request (ignore content)

    const char *resp = 
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 30\r\n"
      "\r\n"
      "{\"message\": \"Hello from mock\"}";
    write(client_fd, resp, strlen(resp));
    close(client_fd);
  }

  close(server_fd);
  exit(0);
  /*#endregion*/
}

Test(stage5, http_step_execution) {
  /*#region*/
  int port_pipe[2];
  pipe(port_pipe);

  // 1. Fork background HTTP server
  pid_t pid = fork();
  if (pid == 0) {
    close(port_pipe[0]); // close read end
    run_mock_http_server(port_pipe[1]);
  }

  close(port_pipe[1]); // close write end
  int port = 0;
  read(port_pipe[0], &port, sizeof(port));
  close(port_pipe[0]);

  // 2. Prepare Workflow YAML dynamically using the assigned port
  char yaml[1024];
  snprintf(yaml, sizeof(yaml),
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"http_workflow\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"http_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"my_http_step\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:%d/users\",\n"
    "            \"headers\": {\n"
    "              \"Content-Type\": \"application/json\",\n"
    "              \"X-User-Id\": \"${{ inputs.user_id }}\"\n"
    "            },\n"
    "            \"body\": {\n"
    "              \"name\": \"${{ inputs.name }}\"\n"
    "            },\n"
    "            \"timeout\": \"5s\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n",
    port
  );

  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Parse and compile workflow
  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  // Prepare root context object
  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  
  // Set inputs
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "user_id"), jsonv_val_str(allocate_jsonv_string(arena, "12345")));
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "name"), jsonv_val_str(allocate_jsonv_string(arena, "Sebastien")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  // Set env
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // Execute workflow
  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Serialize steps outcome to JSON
  char *serialized_str = NULL;
  int32_t ser_status = serialize_jsonv_value(arena, context_val, &serialized_str);
  cr_assert_eq(ser_status, ERR_SUCCESS);

  const char *schema =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"steps\": {\n"
    "      \"type\": \"object\",\n"
    "      \"properties\": {\n"
    "        \"my_http_step\": {\n"
    "          \"type\": \"object\",\n"
    "          \"properties\": {\n"
    "            \"status_code\": { \"type\": \"integer\", \"minimum\": 200, \"maximum\": 200 },\n"
    "            \"body\": {\n"
    "              \"type\": \"object\",\n"
    "              \"properties\": {\n"
    "                \"message\": { \"type\": \"string\" }\n"
    "              },\n"
    "              \"required\": [\"message\"]\n"
    "            }\n"
    "          },\n"
    "          \"required\": [\"status_code\", \"body\"]\n"
    "        }\n"
    "      },\n"
    "      \"required\": [\"my_http_step\"]\n"
    "    }\n"
    "  },\n"
    "  \"required\": [\"steps\"]\n"
    "}";

  cr_assert(validate_json_string(jsonv_arena, serialized_str, schema), "JSON output did not match expected schema");

  int wstatus;
  waitpid(pid, &wstatus, 0);
  arena_destroy(arena);
  /*#endregion*/
}

Test(stage5, plugin_step_execution) {
  /*#region*/
  // 1. Compile mock plugin binary
  system("mkdir -p ./plugins && gcc -O2 tests/fixtures/mock_plugin.c -o ./plugins/mock_plugin");

  // 2. Prepare Workflow YAML
  const char *yaml = 
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"plugin_workflow\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"plugin_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"my_plugin_step\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": {\n"
    "            \"param1\": \"${{ inputs.user_id }}\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

  Arena *arena = arena_create(1024 * 1024);
  cr_assert_not_null(arena);

  // Parse and compile workflow
  WorkflowAST ast;
  int32_t status = parser_parse_buffer(arena, yaml, strlen(yaml), &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  status = compile_workflow(arena, &ast);
  cr_assert_eq(status, ERR_SUCCESS);

  // Prepare root context object
  Jsonv_Arena *jsonv_arena = jsonv_ctx_arena(ast.jsonv_ctx);
  Jsonv_Obj *root_obj = jsonv_obj_new(jsonv_arena, NULL);
  
  // Set inputs
  Jsonv_Obj *inputs_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, inputs_obj, allocate_jsonv_string(arena, "user_id"), jsonv_val_str(allocate_jsonv_string(arena, "12345")));
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "inputs"), jsonv_val_obj(inputs_obj));

  // Set env
  Jsonv_Obj *env_obj = jsonv_obj_new(jsonv_arena, NULL);
  jsonv_obj_set(jsonv_arena, root_obj, allocate_jsonv_string(arena, "env"), jsonv_val_obj(env_obj));

  Jsonv_Value context_val = jsonv_val_obj(root_obj);

  // Execute workflow
  int32_t run_status = run_workflow(arena, &ast, &context_val);
  cr_assert_eq(run_status, ERR_SUCCESS);

  // Serialize steps outcome to JSON
  char *serialized_str = NULL;
  int32_t ser_status = serialize_jsonv_value(arena, context_val, &serialized_str);
  cr_assert_eq(ser_status, ERR_SUCCESS);

  const char *schema =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"steps\": {\n"
    "      \"type\": \"object\",\n"
    "      \"properties\": {\n"
    "        \"my_plugin_step\": {\n"
    "          \"type\": \"object\",\n"
    "          \"properties\": {\n"
    "            \"status_code\": { \"type\": \"integer\", \"minimum\": 0, \"maximum\": 0 },\n"
    "            \"body\": {\n"
    "              \"type\": \"object\",\n"
    "              \"properties\": {\n"
    "                \"plugin_output\": { \"type\": \"string\" }\n"
    "              },\n"
    "              \"required\": [\"plugin_output\"]\n"
    "            }\n"
    "          },\n"
    "          \"required\": [\"status_code\", \"body\"]\n"
    "        }\n"
    "      },\n"
    "      \"required\": [\"my_plugin_step\"]\n"
    "    }\n"
    "  },\n"
    "  \"required\": [\"steps\"]\n"
    "}";

  cr_assert(validate_json_string(jsonv_arena, serialized_str, schema), "JSON output did not match expected schema");

  arena_destroy(arena);
  /*#endregion*/
}
