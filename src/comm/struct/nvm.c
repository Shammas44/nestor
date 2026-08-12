#include "nvm.h"
#include "runner.h"
#include "evaluator.h"
#include "cache.h"
#include "plugin.h"
#include "aho_corasick.h"
#include "transport.h"
#include "ast.h"
#include "crypto.h"
#include "parser.h"
#include "compiler.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/time.h>

typedef struct VMThread VMThread;
struct VMThread {
  uint32_t pc;
  uint32_t sp;
  Jsonv_Value stack[VM_STACK_LIMIT];
  NVMCallFrame *call_stack_top;
  size_t call_stack_depth;
  bool is_active;
  bool is_suspended;
  
  // Job execution state
  ActiveJob *active_job;
  JobNode *target_job;
  
  // Join sync tracking
  bool is_waiting_join;
  bool is_waiting_deps;
  uint8_t join_strategy;
  
  VMThread *next;
};

static char *allocate_jsonv_string(Arena *arena, const char *data, size_t len) {
  /*#region*/
  size_t total_size = sizeof(uint32_t) + len + 1;
  char *buf = na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)len;
  char *str_ptr = buf + sizeof(uint32_t);
  memcpy(str_ptr, data, len);
  str_ptr[len] = '\0';
  return str_ptr;
  /*#endregion*/
}


static uint32_t read_uint32_be_thread(NVMContext *ctx, VMThread *thread) {
  /*#region*/
  if (thread->pc + 4 > ctx->header->code_size) return 0;
  uint32_t val = (ctx->code_segment[thread->pc] << 24) |
                 (ctx->code_segment[thread->pc + 1] << 16) |
                 (ctx->code_segment[thread->pc + 2] << 8) |
                 (ctx->code_segment[thread->pc + 3]);
  thread->pc += 4;
  return val;
  /*#endregion*/
}

static Jsonv_Value get_constant(NVMContext *ctx, uint32_t idx) {
  /*#region*/
  if (!ctx || idx >= ctx->header->const_pool_count) return jsonv_val_null();

  uint32_t offset = 0;
  for (uint32_t i = 0; i < idx; i++) {
    uint32_t len = (ctx->const_pool[offset] << 24) |
                   (ctx->const_pool[offset + 1] << 16) |
                   (ctx->const_pool[offset + 2] << 8) |
                   (ctx->const_pool[offset + 3]);
    offset += 4 + len;
  }

  uint32_t len = (ctx->const_pool[offset] << 24) |
                 (ctx->const_pool[offset + 1] << 16) |
                 (ctx->const_pool[offset + 2] << 8) |
                 (ctx->const_pool[offset + 3]);
  const char *str_data = (const char *)(ctx->const_pool + offset + 4);

  size_t total_size = sizeof(uint32_t) + len + 1;
  char *buf = (char *)na_alloc(ctx->arena, total_size);
  if (!buf) return jsonv_val_null();
  *(uint32_t *)buf = len;
  char *str_ptr = buf + sizeof(uint32_t);
  memcpy(str_ptr, str_data, len);
  str_ptr[len] = '\0';

  return jsonv_val_str(str_ptr);
  /*#endregion*/
}

static void get_active_public_key(uint8_t *pub_key) {
  /*#region*/
  const char *env_pub = getenv("NESTOR_PUBLIC_KEY");
  if (env_pub && strlen(env_pub) == 64) {
    // Parsing Hexadecimal representation of public key from environment
    for (int i = 0; i < 32; i++) {
      unsigned int byte_val;
      sscanf(env_pub + i * 2, "%02x", &byte_val);
      pub_key[i] = (uint8_t)byte_val;
    }
  } else {
    memcpy(pub_key, DEFAULT_PUB_KEY, 32);
  }
  /*#endregion*/
}

int32_t nvm_init_from_file(NVMContext *ctx, Arena *arena, Jsonv_Arena *jsonv_arena, const char *nbc_file_path) {
  /*#region*/
  if (!ctx || !arena || !nbc_file_path) return ERR_INVALID_BOUNDARY;
  memset(ctx, 0, sizeof(NVMContext));

  ctx->arena = arena;
  ctx->jsonv_arena = jsonv_arena;

  int fd = open(nbc_file_path, O_RDONLY);
  if (fd < 0) return ERR_HTTP_TRANSPORT;

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(NVMHeader)) {
    close(fd);
    return ERR_INVALID_BOUNDARY;
  }

  ctx->mapped_size = (size_t)st.st_size;
  ctx->mapped_file = (const uint8_t *)mmap(NULL, ctx->mapped_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (ctx->mapped_file == MAP_FAILED) {
    ctx->mapped_file = NULL;
    return ERR_HTTP_TRANSPORT;
  }

  ctx->header = (const NVMHeader *)ctx->mapped_file;
  if (memcmp(ctx->header->magic, "NEST", 4) != 0) {
    munmap((void *)ctx->mapped_file, ctx->mapped_size);
    ctx->mapped_file = NULL;
    return ERR_INVALID_BOUNDARY;
  }

  // Cryptographic Signature Validation
  if (ctx->header->flags & 1) {
    // Copy the mapped memory to a temp buffer so we can verify the signature with the signature block cleared.
    uint8_t *verify_buf = na_alloc(arena, ctx->mapped_size);
    if (!verify_buf) {
      munmap((void *)ctx->mapped_file, ctx->mapped_size);
      ctx->mapped_file = NULL;
      return ERR_OOM;
    }
    memcpy(verify_buf, ctx->mapped_file, ctx->mapped_size);
    
    // Clear the signature block in the temporary verification buffer.
    memset(((NVMHeader *)verify_buf)->signature, 0, 64);
    
    uint8_t active_pub_key[32];
    get_active_public_key(active_pub_key);
    
    int32_t verify_res = crypto_verify_binary(active_pub_key, verify_buf, ctx->mapped_size, ctx->header->signature);
    if (verify_res != 0) {
      fprintf(stderr, "DEBUG: verify failed with %d, mapped_size=%zu\n", verify_res, ctx->mapped_size);
      munmap((void *)ctx->mapped_file, ctx->mapped_size);
      ctx->mapped_file = NULL;
      return ERR_VM_ILLEGAL_INSTRUCTION;
    }
  }

  ctx->const_pool = ctx->mapped_file + ctx->header->const_pool_offset;
  ctx->code_segment = ctx->mapped_file + ctx->header->code_offset;
  ctx->pc = 0;
  ctx->sp = 0;
  ctx->call_stack_top = NULL;
  ctx->call_stack_depth = 0;

  // Reconstruct WorkflowAST dynamically from embedded metadata in wf_table if present
  if (ctx->header->wf_table_count > 0 && ctx->header->wf_table_offset + sizeof(NVMWorkflowEntry) <= ctx->mapped_size) {
    const NVMWorkflowEntry *entry = (const NVMWorkflowEntry *)(ctx->mapped_file + ctx->header->wf_table_offset);
    if (entry->metadata_offset + entry->metadata_size <= ctx->mapped_size && entry->metadata_size > 0) {
      const char *metadata_str = (const char *)(ctx->mapped_file + entry->metadata_offset);
      WorkflowAST *ast = na_alloc(arena, sizeof(WorkflowAST));
      if (!ast) {
        munmap((void *)ctx->mapped_file, ctx->mapped_size);
        ctx->mapped_file = NULL;
        return ERR_OOM;
      }
      memset(ast, 0, sizeof(WorkflowAST));
      int32_t parse_res = parser_parse_buffer(arena, metadata_str, entry->metadata_size, ast);
      if (parse_res == ERR_SUCCESS) {
        int32_t comp_res = compile_workflow(arena, ast);
        if (comp_res == ERR_SUCCESS) {
          ctx->ast = ast;
        }
      }
    }
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t nvm_execute_loop(NVMContext *ctx) {
  /*#region*/
  if (!ctx || !ctx->code_segment) return ERR_INVALID_BOUNDARY;

  // Let's create the root VM thread
  VMThread *threads_head = na_alloc(ctx->arena, sizeof(VMThread));
  if (!threads_head) return ERR_OOM;
  memset(threads_head, 0, sizeof(VMThread));
  threads_head->pc = ctx->pc;
  threads_head->sp = ctx->sp;
  memcpy(threads_head->stack, ctx->stack, sizeof(ctx->stack));
  threads_head->call_stack_top = ctx->call_stack_top;
  threads_head->call_stack_depth = ctx->call_stack_depth;
  threads_head->is_active = true;

  int32_t ret_val = ERR_SUCCESS;

  if (ctx->context_val) {
    ACNode *ac_root = ac_create_trie(ctx->arena, *(ctx->context_val));
    set_global_ac_root(ac_root);
  }

  if (ctx->ast) {
    ctx->ast->ipc_socket_path[0] = '\0';
    ctx->ipc_listen_fd = -1;
    ctx->ipc_clients_head = NULL;

    snprintf(ctx->ast->ipc_socket_path, sizeof(ctx->ast->ipc_socket_path), "/tmp/nestor_ipc_%p.sock", (void *)ctx->ast);
    unlink(ctx->ast->ipc_socket_path);

    ctx->ipc_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->ipc_listen_fd >= 0) {
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      strncpy(addr.sun_path, ctx->ast->ipc_socket_path, sizeof(addr.sun_path) - 1);
      if (bind(ctx->ipc_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
          listen(ctx->ipc_listen_fd, 5) == 0) {
        int flags = fcntl(ctx->ipc_listen_fd, F_GETFL, 0);
        fcntl(ctx->ipc_listen_fd, F_SETFL, flags | O_NONBLOCK);
      } else {
        close(ctx->ipc_listen_fd);
        ctx->ipc_listen_fd = -1;
        ctx->ast->ipc_socket_path[0] = '\0';
      }
    }
  }

  while (1) {
    if (ctx->ast && ctx->ipc_listen_fd >= 0) {
      while (1) {
        int client_fd = accept(ctx->ipc_listen_fd, NULL, NULL);
        if (client_fd < 0) break;
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        IPCClient *cli = na_alloc(ctx->arena, sizeof(IPCClient));
        if (cli) {
          cli->fd = client_fd;
          cli->len = 0;
          cli->buf[0] = '\0';
          cli->next = ctx->ipc_clients_head;
          ctx->ipc_clients_head = cli;
        } else {
          close(client_fd);
        }
      }
    }

    if (ctx->ast) {
      IPCClient **curr_cli = &ctx->ipc_clients_head;
      while (*curr_cli) {
        IPCClient *cli = *curr_cli;
        char read_buf[256];
        ssize_t n = recv(cli->fd, read_buf, sizeof(read_buf) - 1, 0);
        if (n > 0) {
          if (cli->len + n < (int)sizeof(cli->buf) - 1) {
            memcpy(cli->buf + cli->len, read_buf, n);
            cli->len += n;
            cli->buf[cli->len] = '\0';
          }
          char *newline = strchr(cli->buf, '\n');
          if (newline) {
            *newline = '\0';
            process_ipc_request(ctx->arena, ctx->jsonv_arena, ctx->context_val, cli->fd, cli->buf);
            close(cli->fd);
            *curr_cli = cli->next;
            continue;
          }
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
          close(cli->fd);
          *curr_cli = cli->next;
          continue;
        }
        curr_cli = &(cli->next);
      }
    }

    if (ctx->ast) {
      update_job_states(ctx->ast);
    }

    VMThread *curr = threads_head;
    while (curr) {
      if (curr->target_job && (curr->target_job->execution_state == STATE_SUCCEEDED || curr->target_job->execution_state == STATE_FAILED || curr->target_job->execution_state == STATE_SKIPPED)) {
        curr->is_active = false;
        curr->is_suspended = false;
      }
      if (curr->is_waiting_deps) {
        curr->is_suspended = false;
      }

      if (curr->is_active && !curr->is_suspended) {
        while (curr->pc < ctx->header->code_size) {
          uint8_t opcode = ctx->code_segment[curr->pc++];

          switch (opcode) {
            case OP_NOP:
              break;

            case OP_PUSH_CONST: {
              uint32_t idx = read_uint32_be_thread(ctx, curr);
              if (idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
              curr->stack[curr->sp++] = get_constant(ctx, idx);
              break;
            }

            case OP_POP: {
              if (curr->sp == 0) return ERR_VM_STACK_UNDERFLOW;
              curr->sp--;
              break;
            }

            case OP_RESOLVE: {
              uint32_t path_idx = read_uint32_be_thread(ctx, curr);
              if (path_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              Jsonv_Value path_val = get_constant(ctx, path_idx);

              Jsonv_Value result = jsonv_val_undefined();
              if (ctx->context_val) {
                StringView path_sv = { (const char *)path_val.as.p, jsonv_val_str_len(path_val) };
                int32_t status = evaluate_expression(ctx->arena, path_sv, ctx->jsonv_arena, *(ctx->context_val), &result);
                if (status != ERR_SUCCESS) {
                  return status;
                }
              } else {
                result = path_val;
              }

              if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
              curr->stack[curr->sp++] = result;
              break;
            }

            case OP_JUMP: {
              uint32_t target_offset = read_uint32_be_thread(ctx, curr);
              if (target_offset > ctx->header->code_size) return ERR_VM_INVALID_JUMP;
              curr->pc = target_offset;
              break;
            }

            case OP_JUMP_IF_FALSE: {
              uint32_t target_offset = read_uint32_be_thread(ctx, curr);
              if (target_offset > ctx->header->code_size) return ERR_VM_INVALID_JUMP;
              if (curr->sp == 0) return ERR_VM_STACK_UNDERFLOW;
              Jsonv_Value val = curr->stack[--curr->sp];
              bool is_falsy = (val.tag == JSONV_VAL_BOOLEAN && !val.as.boolean) ||
                              (val.tag == JSONV_VAL_NULL) ||
                              (val.tag == JSONV_VAL_UNDEFINED);
              if (is_falsy) {
                curr->pc = target_offset;
              }
              break;
            }

            case OP_CALL_PROVIDER: {
              uint32_t prov_op_idx = read_uint32_be_thread(ctx, curr);
              if (prov_op_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;

              if (ctx->ast && ctx->context_val) {
                Jsonv_Value job_id_val = get_constant(ctx, prov_op_idx);
                StringView job_id_sv = { (const char *)job_id_val.as.p, jsonv_val_str_len(job_id_val) };

                JobNode *job = ctx->ast->jobs_head;
                while (job) {
                  if (sv_compare(job->id, job_id_sv) == 0) break;
                  job = job->next_sorted;
                }

                if (job) {
                  if (job->execution_state == STATE_SKIPPED) {
                    if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                    curr->stack[curr->sp++] = jsonv_val_bool(true);
                    break;
                  }

                  if (job->type != NODE_JOIN) {
                    bool deps_ready = true;
                    for (size_t d = 0; d < job->dependency_count; d++) {
                      JobNode *dep = job->depends_on_nodes[d];
                      if (dep && (dep->execution_state == STATE_PENDING || dep->execution_state == STATE_RUNNING)) {
                        deps_ready = false;
                        break;
                      }
                    }

                    if (!deps_ready) {
                      curr->is_suspended = true;
                      curr->is_waiting_deps = true;
                      curr->pc -= 5;
                      break;
                    }
                    curr->is_waiting_deps = false;
                  }

                  ActiveJob *aj = na_alloc(ctx->arena, sizeof(ActiveJob));
                  if (!aj) return ERR_OOM;
                  memset(aj, 0, sizeof(ActiveJob));
                  aj->job = job;

                  if (check_and_apply_cache(ctx->arena, ctx->jsonv_arena, ctx->ast, ctx->context_val, job, aj)) {
                    job->execution_state = STATE_SUCCEEDED;
                    if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                    curr->stack[curr->sp++] = jsonv_val_bool(true);
                    break;
                  }

                  // 1. Create and register job_outcome_obj in "jobs" first
                  char *job_id_cstr = allocate_jsonv_string(ctx->arena, job->id.data, job->id.length);
                  Jsonv_Obj *job_outcome_obj = jsonv_obj_new(ctx->jsonv_arena, NULL);
                  Jsonv_Value jobs_val_obj;
                  if (jsonv_obj_get(ctx->context_val->as.p, "jobs", &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
                    jsonv_obj_set(ctx->jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
                  }

                  // 2. Evaluate job variables first pass (for private variables)
                  Jsonv_Value local_vars;
                  local_vars.tag = JSONV_VAL_OBJ;
                  local_vars.as.p = jsonv_obj_new(ctx->jsonv_arena, NULL);
                  push_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                  evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, job, local_vars);

                  if (job->type == NODE_IF || job->type == NODE_SWITCH || job->type == NODE_FORK || job->type == NODE_JOIN) {
                    pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                    return ERR_VM_ILLEGAL_INSTRUCTION;
                  } else if (job->type == NODE_TRANSFORM) {
                    job->execution_state = STATE_RUNNING;
                    Jsonv_Value transform_res = jsonv_val_undefined();
                    int32_t status = evaluate_expression(ctx->arena, job->spec.transform.expression, ctx->jsonv_arena, *(ctx->context_val), &transform_res);
                    if (status != ERR_SUCCESS) {
                      pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                      job->execution_state = STATE_FAILED;
                      return status;
                    }

                    jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, "outputs", transform_res);
                    jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, "output", transform_res);
                    jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, "result", transform_res);

                    // 3. Evaluate job variables second pass (for public outcomes)
                    evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, job, local_vars);

                    pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                    job->execution_state = STATE_SUCCEEDED;
                    store_job_in_cache(ctx->arena, ctx->ast, *(ctx->context_val), job, NULL);

                    if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                    curr->stack[curr->sp++] = jsonv_val_bool(true);
                  } else if (job->type == NODE_EXPORT) {
                    job->execution_state = STATE_RUNNING;
                    StringView resolved_file_path = {0};
                    int32_t status = resolve_string(ctx->arena, job->spec.export_node.file_path, ctx->jsonv_arena, *(ctx->context_val), &resolved_file_path);
                    if (status != ERR_SUCCESS) {
                      pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                      job->execution_state = STATE_FAILED;
                      return status;
                    }
                    char *file_path_cstr = sv_to_cstring(ctx->arena, resolved_file_path);

                    Jsonv_Value data_res = resolve_json_value(ctx->arena, job->spec.export_node.data_val, ctx->jsonv_arena, *(ctx->context_val));
                    char *serialized_data = NULL;
                    serialize_jsonv_value(ctx->arena, data_res, &serialized_data);
                    if (serialized_data && file_path_cstr) {
                      FILE *f = fopen(file_path_cstr, "w");
                      if (f) {
                        fputs(serialized_data, f);
                        fclose(f);
                      } else {
                        pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                        job->execution_state = STATE_FAILED;
                        return ERR_HTTP_TRANSPORT;
                      }
                    }

                    jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, "status", jsonv_val_str(allocate_jsonv_string(ctx->arena, "success", 7)));

                    // Evaluate job variables second pass (for public outcomes)
                    evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, job, local_vars);

                    pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                    job->execution_state = STATE_SUCCEEDED;
                    if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                    curr->stack[curr->sp++] = jsonv_val_bool(true);
                  } else {
                    if (job->type == NODE_WAIT_SIGNAL) {
                      Jsonv_Value resolved_corr = jsonv_val_undefined();
                      int32_t status = evaluate_expression(ctx->arena, job->spec.wait_signal.correlation_id, ctx->jsonv_arena, *(ctx->context_val), &resolved_corr);
                      if (status != ERR_SUCCESS) {
                        pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                        job->execution_state = STATE_FAILED;
                        return status;
                      }

                      Jsonv_Value incoming_corr = jsonv_val_undefined();
                      bool incoming_matched = false;
                      Jsonv_Value inputs_val;
                      if (jsonv_obj_get(ctx->context_val->as.p, "inputs", &inputs_val) && inputs_val.tag == JSONV_VAL_OBJ) {
                        if (jsonv_obj_get(inputs_val.as.p, "correlation_id", &incoming_corr)) {
                          if (jsonv_values_equal(resolved_corr, incoming_corr)) {
                            incoming_matched = true;
                          }
                        }
                      }

                      if (!incoming_matched) {
                        job->execution_state = STATE_SUSPENDED;
                        fprintf(stderr, "Workflow suspended at wait_signal job '%.*s'.\n", (int)job->id.length, job->id.data);
                        pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);

                        char state_path[270];
                        char lock_path[270];
                        get_state_paths(ctx->ast, state_path, sizeof(state_path), lock_path, sizeof(lock_path));
                        save_tfstate(ctx->arena, state_path, *(ctx->context_val));

                        return ERR_SUCCESS;
                      }
                    }

                    aj->local_vars = local_vars;

                    aj->steps_state_obj.tag = JSONV_VAL_OBJ;
                    aj->steps_state_obj.as.p = jsonv_obj_new(ctx->jsonv_arena, NULL);

                    char *k_steps = allocate_jsonv_string(ctx->arena, "steps", 5);
                    jsonv_obj_set(ctx->jsonv_arena, ctx->context_val->as.p, k_steps, aj->steps_state_obj);
                    jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, k_steps, aj->steps_state_obj);

                    if (job->type == NODE_LOOP) {
                      aj->is_loop = true;
                      aj->loop_iter = 0;
                      aj->max_iterations = job->spec.loop_node.max_iterations;
                      if (aj->max_iterations == 0) {
                        aj->max_iterations = 1000;
                      }
                      aj->curr_step = job->spec.loop_node.steps_head;
                      aj->loop_arena = arena_create(ctx->arena->default_chunk_size);
                      
                      int32_t status = start_loop_iteration(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj);
                      if (status != ERR_SUCCESS) {
                        pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                        return status;
                      }
                    } else if (job->type == NODE_WAIT_SIGNAL) {
                      aj->curr_step = job->spec.wait_signal.steps_head;
                    } else {
                      aj->curr_step = job->spec.task.steps_head;
                    }

                    if (aj->curr_step == NULL) {
                      job->execution_state = STATE_SUCCEEDED;
                      char *k_status = allocate_jsonv_string(ctx->arena, "status", 6);
                      jsonv_obj_set(ctx->jsonv_arena, job_outcome_obj, k_status, jsonv_val_str(allocate_jsonv_string(ctx->arena, "success", 7)));
                      
                      // Evaluate job variables second pass (for public outcomes)
                      evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, job, local_vars);

                      pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);

                      if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                      curr->stack[curr->sp++] = jsonv_val_bool(true);
                    } else {
                      job->execution_state = STATE_RUNNING;
                      curr->active_job = aj;
                      curr->is_suspended = true;

                      int32_t status = advance_active_job(ctx->arena, ctx->jsonv_arena, ctx->ast, ctx->context_val, aj, ctx->transport);
                      if (status != ERR_SUCCESS) {
                        pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                        return status;
                      }

                      pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                      break;
                    }
                  }
                }
              } else {
                if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
                curr->stack[curr->sp++] = jsonv_val_bool(true);
              }
              break;
            }

            case OP_CALL_WORKFLOW: {
              uint32_t wf_idx = read_uint32_be_thread(ctx, curr);
              (void)wf_idx;
              if (curr->call_stack_depth >= VM_CALL_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
              NVMCallFrame *frame = (NVMCallFrame *)na_alloc(ctx->arena, sizeof(NVMCallFrame));
              if (!frame) return ERR_OOM;
              frame->return_pc = curr->pc;
              frame->prev = curr->call_stack_top;
              curr->call_stack_top = frame;
              curr->call_stack_depth++;
              break;
            }

            case OP_RETURN: {
              if (curr->call_stack_top) {
                curr->pc = curr->call_stack_top->return_pc;
                curr->call_stack_top = curr->call_stack_top->prev;
                curr->call_stack_depth--;
              } else {
                curr->is_active = false;
              }
              break;
            }

            case OP_FORK: {
              if (curr->pc >= ctx->header->code_size) return ERR_VM_OUT_OF_BOUNDS;
              uint8_t count = ctx->code_segment[curr->pc++];

              for (uint8_t c = 0; c < count; c++) {
                uint32_t target_pc = read_uint32_be_thread(ctx, curr);

                VMThread *new_thread = na_alloc(ctx->arena, sizeof(VMThread));
                if (!new_thread) return ERR_OOM;
                memset(new_thread, 0, sizeof(VMThread));
                new_thread->pc = target_pc;
                new_thread->sp = curr->sp;
                memcpy(new_thread->stack, curr->stack, sizeof(curr->stack));
                new_thread->call_stack_top = curr->call_stack_top;
                new_thread->call_stack_depth = curr->call_stack_depth;
                new_thread->is_active = true;

                new_thread->next = threads_head;
                threads_head = new_thread;
              }
              break;
            }

            case OP_JOIN: {
              if (curr->pc + 5 > ctx->header->code_size) return ERR_VM_OUT_OF_BOUNDS;
              uint8_t strategy = ctx->code_segment[curr->pc++];
              uint32_t join_job_idx = (ctx->code_segment[curr->pc] << 24) |
                                      (ctx->code_segment[curr->pc + 1] << 16) |
                                      (ctx->code_segment[curr->pc + 2] << 8) |
                                      (ctx->code_segment[curr->pc + 3]);
              curr->pc += 4;

              if (ctx->ast && ctx->context_val) {
                Jsonv_Value job_id_val = get_constant(ctx, join_job_idx);
                StringView job_id_sv = { (const char *)job_id_val.as.p, jsonv_val_str_len(job_id_val) };

                JobNode *job = ctx->ast->jobs_head;
                while (job) {
                  if (sv_compare(job->id, job_id_sv) == 0) break;
                  job = job->next_sorted;
                }

                if (job) {
                  curr->target_job = job;
                  if (job->execution_state == STATE_PENDING) {
                    job->execution_state = STATE_RUNNING;
                  }
                  
                  char *job_id_cstr = allocate_jsonv_string(ctx->arena, job->id.data, job->id.length);
                  Jsonv_Obj *job_outcome_obj = jsonv_obj_new(ctx->jsonv_arena, NULL);
                  Jsonv_Value jobs_val_obj;
                  if (jsonv_obj_get(ctx->context_val->as.p, "jobs", &jobs_val_obj) && jobs_val_obj.tag == JSONV_VAL_OBJ) {
                    jsonv_obj_set(ctx->jsonv_arena, jobs_val_obj.as.p, job_id_cstr, jsonv_val_obj(job_outcome_obj));
                  }
                  
                  Jsonv_Value local_vars;
                  local_vars.tag = JSONV_VAL_OBJ;
                  local_vars.as.p = jsonv_obj_new(ctx->jsonv_arena, NULL);
                  push_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                  evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, job, local_vars);
                  pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
                }
              }

              curr->is_suspended = true;
              curr->is_waiting_join = true;
              curr->join_strategy = strategy;
              break;
            }

            case OP_LOAD_ENV: {
              uint32_t key_idx = read_uint32_be_thread(ctx, curr);
              if (key_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              Jsonv_Value key_val = get_constant(ctx, key_idx);
              if (key_val.tag != JSONV_VAL_STRING) return ERR_VM_ILLEGAL_INSTRUCTION;
              
              const char *key_str = key_val.as.p;
              Jsonv_Value val = jsonv_val_null();
              
              bool found = false;
              if (ctx->context_val && ctx->context_val->tag == JSONV_VAL_OBJ) {
                Jsonv_Value env_obj;
                if (jsonv_obj_get(ctx->context_val->as.p, "env", &env_obj) && env_obj.tag == JSONV_VAL_OBJ) {
                  if (jsonv_obj_get(env_obj.as.p, key_str, &val)) {
                    found = true;
                  }
                }
              }
              
              if (!found) {
                const char *env_val = getenv(key_str);
                if (env_val) {
                  char *env_val_cstr = allocate_jsonv_string(ctx->arena, env_val, strlen(env_val));
                  val = jsonv_val_str(env_val_cstr);
                }
              }
              
              if (curr->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
              curr->stack[curr->sp++] = val;
              break;
            }

            case OP_STORE_VAR: {
              if (curr->sp == 0) return ERR_VM_STACK_UNDERFLOW;
              uint32_t key_idx = read_uint32_be_thread(ctx, curr);
              if (key_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              Jsonv_Value key_val = get_constant(ctx, key_idx);
              if (key_val.tag != JSONV_VAL_STRING) return ERR_VM_ILLEGAL_INSTRUCTION;
              
              const char *key_str = key_val.as.p;
              Jsonv_Value val = curr->stack[--curr->sp];
              
              if (ctx->context_val && ctx->context_val->tag == JSONV_VAL_OBJ) {
                jsonv_obj_set(ctx->jsonv_arena, ctx->context_val->as.p, key_str, val);
              }
              break;
            }

            case OP_COMPLETE_JOB: {
              uint32_t job_idx = read_uint32_be_thread(ctx, curr);
              if (job_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              if (ctx->ast) {
                Jsonv_Value job_id_val = get_constant(ctx, job_idx);
                StringView job_id_sv = { (const char *)job_id_val.as.p, jsonv_val_str_len(job_id_val) };
                JobNode *job = ctx->ast->jobs_head;
                while (job) {
                  if (sv_compare(job->id, job_id_sv) == 0) {
                    if (job->execution_state == STATE_PENDING || job->execution_state == STATE_RUNNING) {
                      job->execution_state = STATE_SUCCEEDED;
                    }
                    break;
                  }
                  job = job->next_sorted;
                }
              }
              break;
            }

            case OP_SKIP_JOB: {
              uint32_t job_idx = read_uint32_be_thread(ctx, curr);
              if (job_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
              if (ctx->ast) {
                Jsonv_Value job_id_val = get_constant(ctx, job_idx);
                StringView job_id_sv = { (const char *)job_id_val.as.p, jsonv_val_str_len(job_id_val) };
                mark_job_skipped(ctx->ast, job_id_sv);
              }
              break;
            }

            default:
              return ERR_VM_ILLEGAL_INSTRUCTION;
          }

          if (curr->is_suspended || !curr->is_active) {
            break;
          }
        }
        if (curr->pc >= ctx->header->code_size) {
          curr->is_active = false;
        }
        if (curr->target_job && (curr->target_job->execution_state == STATE_SUCCEEDED || curr->target_job->execution_state == STATE_FAILED || curr->target_job->execution_state == STATE_SKIPPED)) {
          if (!curr->is_suspended) {
            curr->is_active = false;
          }
        }
      }
      curr = curr->next;
    }

    bool active_remaining = false;
    VMThread *t = threads_head;
    while (t) {
      if (t->is_active) {
        active_remaining = true;
        break;
      }
      t = t->next;
    }

    if (!active_remaining) break;

    VMThread *t_retry = threads_head;
    while (t_retry) {
      if (t_retry->is_active && t_retry->is_suspended && t_retry->active_job && t_retry->active_job->is_waiting_retry) {
        ActiveJob *aj = t_retry->active_job;
        struct timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > aj->next_retry_time.tv_sec ||
            (now.tv_sec == aj->next_retry_time.tv_sec && now.tv_usec >= aj->next_retry_time.tv_usec)) {
          aj->is_waiting_retry = false;
          push_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
          int32_t status = advance_active_job(ctx->arena, ctx->jsonv_arena, ctx->ast, ctx->context_val, aj, ctx->transport);
          pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
          if (status != ERR_SUCCESS) {
            return status;
          }
        }
      }
      t_retry = t_retry->next;
    }

    int still_running = 0;
    if (ctx->transport) {
      ctx->transport->ops->poll_requests(ctx->transport, &still_running);
    }

    VMThread *t_http = threads_head;
    while (t_http) {
      if (t_http->is_active && t_http->is_suspended && t_http->active_job && t_http->active_job->easy_handle) {
        ActiveJob *aj = t_http->active_job;
        push_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
        long status_code = 0;
        bool completed = false;
        bool error = false;
        int32_t status = ctx->transport->ops->check_completed(ctx->transport, ctx->arena, ctx->jsonv_arena, aj->easy_handle, &aj->resp_buf, &status_code, &completed, &error);
        if (status == ERR_SUCCESS && completed) {
          Arena *eff_arena = aj->loop_arena ? aj->loop_arena : ctx->arena;
          if (!error) {
            if (status_code >= 400 && handle_step_failure(eff_arena, ctx->jsonv_arena, ctx->context_val, aj, "HTTP status >= 400")) {
              ctx->transport->ops->cleanup_request(ctx->transport, aj->easy_handle);
              aj->easy_handle = NULL;
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_http = t_http->next;
              continue;
            }
            if (status_code >= 400 && aj->curr_step->has_on_error) {
              ctx->transport->ops->cleanup_request(ctx->transport, aj->easy_handle);
              aj->easy_handle = NULL;
              int32_t comp_status = apply_step_fallback(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj, ctx->ast, ctx->transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
                return comp_status;
              }
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_http = t_http->next;
              continue;
            }
            int32_t comp_status = complete_http_step_async(ctx->arena, ctx->jsonv_arena, aj, status_code);
            if (comp_status != ERR_SUCCESS) {
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              return comp_status;
            }
            if (status_code >= 400) {
              aj->job->execution_state = STATE_FAILED;
            }
          } else {
            if (handle_step_failure(eff_arena, ctx->jsonv_arena, ctx->context_val, aj, "HTTP transport error")) {
              ctx->transport->ops->cleanup_request(ctx->transport, aj->easy_handle);
              aj->easy_handle = NULL;
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_http = t_http->next;
              continue;
            }
            if (aj->curr_step->has_on_error) {
              ctx->transport->ops->cleanup_request(ctx->transport, aj->easy_handle);
              aj->easy_handle = NULL;
              int32_t comp_status = apply_step_fallback(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj, ctx->ast, ctx->transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
                return comp_status;
              }
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_http = t_http->next;
              continue;
            }
            aj->job->execution_state = STATE_FAILED;
          }

          ctx->transport->ops->cleanup_request(ctx->transport, aj->easy_handle);
          aj->easy_handle = NULL;
          aj->curr_step_retry_attempt = 0;

          evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj->job, aj->local_vars);

          aj->curr_step = aj->curr_step->next;

          int32_t adv_status = advance_active_job(ctx->arena, ctx->jsonv_arena, ctx->ast, ctx->context_val, aj, ctx->transport);
          pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
          if (adv_status != ERR_SUCCESS) {
            return adv_status;
          }
        } else {
          pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
        }
      }
      t_http = t_http->next;
    }

    VMThread *t_proc = threads_head;
    while (t_proc) {
      if (t_proc->is_active && t_proc->is_suspended && t_proc->active_job && t_proc->active_job->plugin_exec.child_pid != 0) {
        ActiveJob *aj = t_proc->active_job;
        push_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
        bool finished = false;
        long exit_code = 0;
        Arena *eff_arena = aj->loop_arena ? aj->loop_arena : ctx->arena;
        plugin_poll(&aj->plugin_exec, eff_arena, ctx->jsonv_arena, ctx->context_val, aj->curr_step, &finished, &exit_code);
        if (finished) {
          if (exit_code == -4) {
            if (handle_step_failure(eff_arena, ctx->jsonv_arena, ctx->context_val, aj, "Plugin timeout")) {
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_proc = t_proc->next;
              continue;
            }
            if (aj->curr_step->has_on_error) {
              int32_t comp_status = apply_step_fallback(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj, ctx->ast, ctx->transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
                return comp_status;
              }
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_proc = t_proc->next;
              continue;
            }
            complete_plugin_step_async(ctx->arena, ctx->jsonv_arena, aj, -4);
            aj->job->execution_state = STATE_FAILED;
          } else {
            if (exit_code != 0 && handle_step_failure(eff_arena, ctx->jsonv_arena, ctx->context_val, aj, "Plugin exited with non-zero code")) {
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_proc = t_proc->next;
              continue;
            }
            if (exit_code != 0 && aj->curr_step->has_on_error) {
              int32_t comp_status = apply_step_fallback(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj, ctx->ast, ctx->transport);
              if (comp_status != ERR_SUCCESS) {
                pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
                return comp_status;
              }
              pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
              t_proc = t_proc->next;
              continue;
            }
            complete_plugin_step_async(ctx->arena, ctx->jsonv_arena, aj, exit_code);
            aj->curr_step_retry_attempt = 0;

            evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, aj->job, aj->local_vars);

            aj->curr_step = aj->curr_step->next;

            int32_t adv_status = advance_active_job(ctx->arena, ctx->jsonv_arena, ctx->ast, ctx->context_val, aj, ctx->transport);
            pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
            if (adv_status != ERR_SUCCESS) {
              return adv_status;
            }
          }
        } else {
          pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), aj->local_vars);
        }
      }
      t_proc = t_proc->next;
    }

    VMThread *t_chk = threads_head;
    while (t_chk) {
      if (t_chk->is_active && t_chk->is_suspended && t_chk->active_job) {
        ActiveJob *aj = t_chk->active_job;
        if (aj->job->execution_state == STATE_SUCCEEDED || aj->job->execution_state == STATE_FAILED) {
          t_chk->is_suspended = false;
          t_chk->active_job = NULL;
          if (aj->job->execution_state == STATE_SUCCEEDED) {
            store_job_in_cache(ctx->arena, ctx->ast, *(ctx->context_val), aj->job, aj);
          }
          if (t_chk->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
          t_chk->stack[t_chk->sp++] = jsonv_val_bool(aj->job->execution_state == STATE_SUCCEEDED);
        }
      }
      t_chk = t_chk->next;
    }

    VMThread *t_join = threads_head;
    while (t_join) {
      if (t_join->is_active && t_join->is_suspended && t_join->is_waiting_join && t_join->target_job) {
        JobNode *join_job = t_join->target_job;

        if (join_job) {
          size_t num_succeeded = 0;

          size_t num_skipped = 0;
          size_t num_completed = 0;
          size_t num_satisfied = 0;

          for (size_t d = 0; d < join_job->dependency_count; d++) {
            JobNode *dep = join_job->depends_on_nodes[d];
            if (dep->execution_state == STATE_SUCCEEDED) {
              num_succeeded++;
              num_completed++;
            } else if (dep->execution_state == STATE_FAILED) {
              num_completed++;
            } else if (dep->execution_state == STATE_SKIPPED) {
              num_skipped++;
              num_completed++;
            }

            if (dep->execution_state == STATE_SUCCEEDED ||
                dep->execution_state == STATE_FAILED ||
                dep->execution_state == STATE_SKIPPED) {
              if (is_edge_satisfied(join_job, d, dep)) {
                num_satisfied++;
              }
            }
          }

          bool join_ok = false;
          bool join_evaluated = false;

          if (join_job->depends_on_conditions != NULL) {
            if (t_join->join_strategy == 1) {
              if (num_completed == join_job->dependency_count) {
                join_ok = (num_satisfied == join_job->dependency_count);
                join_evaluated = true;
              }
            } else if (t_join->join_strategy == 2) {
              if (num_satisfied > 0) {
                join_ok = true;
                join_evaluated = true;
              } else if (num_completed == join_job->dependency_count) {
                join_ok = false;
                join_evaluated = true;
              }
            } else if (t_join->join_strategy == 3) {
              size_t n_req = join_job->spec.join_node.n_required;
              if (num_satisfied >= n_req) {
                join_ok = true;
                join_evaluated = true;
              } else if (num_completed == join_job->dependency_count) {
                join_ok = false;
                join_evaluated = true;
              }
            }
          } else {
            if (t_join->join_strategy == 1) {
              if (num_completed == join_job->dependency_count) {
                join_ok = (num_succeeded + num_skipped == join_job->dependency_count);
                join_evaluated = true;
              }
            } else if (t_join->join_strategy == 2) {
              if (num_succeeded > 0) {
                join_ok = true;
                join_evaluated = true;
              } else if (num_completed == join_job->dependency_count) {
                join_ok = false;
                join_evaluated = true;
              }
            } else if (t_join->join_strategy == 3) {
              size_t n_req = join_job->spec.join_node.n_required;
              if (num_succeeded >= n_req) {
                join_ok = true;
                join_evaluated = true;
              } else if (num_completed == join_job->dependency_count) {
                join_ok = false;
                join_evaluated = true;
              }
            }
          }

          if (join_evaluated) {
            join_job->execution_state = join_ok ? STATE_SUCCEEDED : STATE_FAILED;
            
            // We need to evaluate the variables again for public outcomes
            Jsonv_Value local_vars;
            local_vars.tag = JSONV_VAL_OBJ;
            local_vars.as.p = jsonv_obj_new(ctx->jsonv_arena, NULL);
            push_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);
            evaluate_job_variables(ctx->arena, ctx->jsonv_arena, ctx->context_val, join_job, local_vars);
            pop_local_vars(ctx->jsonv_arena, *(ctx->context_val), local_vars);

            if (join_ok) {
              t_join->is_suspended = false;
              t_join->is_waiting_join = false;
              t_join->target_job = NULL;
              if (t_join->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
              t_join->stack[t_join->sp++] = jsonv_val_bool(true);
            } else {
              t_join->is_active = false;
              t_join->is_suspended = false;
              t_join->target_job = NULL;
            }
          }
        }
      }
      t_join = t_join->next;
    }

    bool suspended_on_wait_signal = false;
    VMThread *t_susp = threads_head;
    while (t_susp) {
      if (t_susp->is_active && t_susp->is_suspended && t_susp->active_job && t_susp->active_job->job->type == NODE_WAIT_SIGNAL) {
        ActiveJob *aj = t_susp->active_job;
        if (aj->job->execution_state == STATE_SUSPENDED) {
          suspended_on_wait_signal = true;
        }
      }
      t_susp = t_susp->next;
    }

    if (suspended_on_wait_signal) {
      if (ctx->ast && ctx->context_val) {
        char state_path[270];
        char lock_path[270];
        get_state_paths(ctx->ast, state_path, sizeof(state_path), lock_path, sizeof(lock_path));
        save_tfstate(ctx->arena, state_path, *(ctx->context_val));
      }
      return ERR_SUCCESS;
    }

    bool has_active = false;
    VMThread *t_act = threads_head;
    while (t_act) {
      if (t_act->is_active) {
        has_active = true;
        break;
      }
      t_act = t_act->next;
    }
    if (!has_active) {
      break;
    }

    usleep(1000);
  }

  ctx->pc = threads_head->pc;
  ctx->sp = threads_head->sp;
  memcpy(ctx->stack, threads_head->stack, sizeof(ctx->stack));
  ctx->call_stack_top = threads_head->call_stack_top;
  ctx->call_stack_depth = threads_head->call_stack_depth;

  if (ret_val == ERR_SUCCESS && ctx->ast) {
    JobNode *j = ctx->ast->jobs_head;
    while (j) {
      if (j->execution_state == STATE_FAILED) {
        ret_val = ERR_HTTP_TRANSPORT;
        break;
      }
      j = j->next_sorted;
    }
  }

  if (ret_val == ERR_SUCCESS && ctx->ast) {
    JobNode *ret_job = ctx->ast->jobs_head;
    while (ret_job) {
      if (ret_job->execution_state == STATE_SUCCEEDED && ret_job->return_expr.length > 0) {
        Jsonv_Context *temp_ctx = jsonv_ctx_new(ctx->jsonv_arena, NULL, NULL);
        if (temp_ctx) {
          char *expr_str = sv_to_cstring(ctx->arena, ret_job->return_expr);
          if (expr_str && jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)expr_str)) {
            Jsonv_Value parsed_val;
            jsonv_ctx_get_value(temp_ctx, &parsed_val);
            Jsonv_Value resolved_val = resolve_json_value(ctx->arena, parsed_val, ctx->jsonv_arena, *(ctx->context_val));
            
            char *k_outputs = allocate_jsonv_string(ctx->arena, "outputs", 7);
            jsonv_obj_set(ctx->jsonv_arena, ctx->context_val->as.p, k_outputs, resolved_val);
          }
        }
      }
      ret_job = ret_job->next_sorted;
    }
  }

  return ret_val;
  /*#endregion*/
}

void nvm_close(NVMContext *ctx) {
  /*#region*/
  if (ctx) {
    if (ctx->ast && ctx->ipc_listen_fd >= 0) {
      IPCClient *cli_c = ctx->ipc_clients_head;
      while (cli_c) {
        close(cli_c->fd);
        cli_c = cli_c->next;
      }
      close(ctx->ipc_listen_fd);
      ctx->ipc_listen_fd = -1;
      if (ctx->ast->ipc_socket_path[0] != '\0') {
        unlink(ctx->ast->ipc_socket_path);
      }
    }
    if (ctx->mapped_file) {
      munmap((void *)ctx->mapped_file, ctx->mapped_size);
      ctx->mapped_file = NULL;
    }
  }
  /*#endregion*/
}
