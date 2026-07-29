#include "testutils.h"
#include "nestor.h"
#include "parser.h"
#include "compiler.h"
#include <criterion/criterion.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

static void run_concurrency_mock_server(int write_fd) {
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

  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(1);
  }

  struct sockaddr_in bound_addr;
  socklen_t addr_len = sizeof(bound_addr);
  if (getsockname(server_fd, (struct sockaddr *)&bound_addr, &addr_len) < 0) {
    perror("getsockname failed");
    close(server_fd);
    exit(1);
  }
  int port = ntohs(bound_addr.sin_port);

  write(write_fd, &port, sizeof(port));
  close(write_fd);

  int client_fds[3];
  for (int i = 0; i < 3; i++) {
    client_fds[i] = accept(server_fd, NULL, NULL);
    if (client_fds[i] < 0) {
      perror("accept failed");
      exit(1);
    }
  }

  char bufs[3][1024];
  int fast_idx = -1;
  for (int i = 0; i < 3; i++) {
    memset(bufs[i], 0, sizeof(bufs[i]));
    read(client_fds[i], bufs[i], sizeof(bufs[i]) - 1);
    if (strstr(bufs[i], "/fast")) {
      fast_idx = i;
    }
  }

  if (fast_idx >= 0) {
    const char *resp = 
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 28\r\n"
      "\r\n"
      "{\"status\": \"fast_completed\"}";
    write(client_fds[fast_idx], resp, strlen(resp));
    close(client_fds[fast_idx]);
  }

  usleep(200000); // 200ms sleep for other connections

  for (int i = 0; i < 3; i++) {
    if (i != fast_idx) {
      const char *resp = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 28\r\n"
        "\r\n"
        "{\"status\": \"slow_completed\"}";
      write(client_fds[i], resp, strlen(resp));
      close(client_fds[i]);
    }
  }

  close(server_fd);
  exit(0);
  /*#endregion*/
}

TIMED_TEST(stage7, parallel_fork_join_any, init, fini) {
  /*#region*/
  int port_pipe[2];
  pipe(port_pipe);

  pid_t pid = fork();
  if (pid == 0) {
    close(port_pipe[0]);
    run_concurrency_mock_server(port_pipe[1]);
  }

  close(port_pipe[1]);
  int port = 0;
  read(port_pipe[0], &port, sizeof(port));
  close(port_pipe[0]);

  char yaml[2048];
  snprintf(yaml, sizeof(yaml),
    "{\n"
    "  \"version\": \"2.0.0\",\n"
    "  \"name\": \"Fork Join Any Test\",\n"
    "  \"on\": { \"manual\": {} },\n"
    "  \"jobs\": {\n"
    "    \"fork_trigger\": {\n"
    "      \"type\": \"fork\",\n"
    "      \"branches\": [\"fast_job\", \"slow1_job\", \"slow2_job\"]\n"
    "    },\n"
    "    \"fast_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step_fast\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:%d/fast\",\n"
    "            \"timeout\": \"5s\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"slow1_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step_slow1\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:%d/slow1\",\n"
    "            \"timeout\": \"5s\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"slow2_job\": {\n"
    "      \"type\": \"task\",\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"step_slow2\",\n"
    "          \"http\": {\n"
    "            \"method\": \"POST\",\n"
    "            \"url\": \"http://127.0.0.1:%d/slow2\",\n"
    "            \"timeout\": \"5s\"\n"
    "          }\n"
    "        }\n"
    "      ]\n"
    "    },\n"
    "    \"join_barrier\": {\n"
    "      \"type\": \"join\",\n"
    "      \"strategy\": \"any\",\n"
    "      \"depends_on\": [\"fast_job\", \"slow1_job\", \"slow2_job\"]\n"
    "    },\n"
    "    \"downstream\": {\n"
    "      \"type\": \"task\",\n"
    "      \"depends_on\": [\"join_barrier\"],\n"
    "      \"steps\": [\n"
    "        {\n"
    "          \"id\": \"downstream_step\",\n"
    "          \"uses\": \"./plugins/mock_plugin\",\n"
    "          \"with\": {}\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n",
    port, port, port
  );

  Arena *arena = arena_create(2 * 1024 * 1024);
  cr_assert_not_null(arena);

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

  assert_job_state(&ast, "fork_trigger", STATE_SUCCEEDED);
  assert_job_state(&ast, "fast_job", STATE_SUCCEEDED);
  assert_job_state(&ast, "slow1_job", STATE_SUCCEEDED);
  assert_job_state(&ast, "slow2_job", STATE_SUCCEEDED);
  assert_job_state(&ast, "join_barrier", STATE_SUCCEEDED);
  assert_job_state(&ast, "downstream", STATE_SUCCEEDED);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  arena_destroy(arena);
  /*#endregion*/
}
END_TIMED_TEST
