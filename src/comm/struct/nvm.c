#include "nvm.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t read_uint32_be(NVMContext *ctx) {
  /*#region*/
  if (ctx->pc + 4 > ctx->header->code_size) return 0;
  uint32_t val = (ctx->code_segment[ctx->pc] << 24) |
                 (ctx->code_segment[ctx->pc + 1] << 16) |
                 (ctx->code_segment[ctx->pc + 2] << 8) |
                 (ctx->code_segment[ctx->pc + 3]);
  ctx->pc += 4;
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

  ctx->const_pool = ctx->mapped_file + ctx->header->const_pool_offset;
  ctx->code_segment = ctx->mapped_file + ctx->header->code_offset;
  ctx->pc = 0;
  ctx->sp = 0;
  ctx->call_stack_top = NULL;
  ctx->call_stack_depth = 0;

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t nvm_execute_loop(NVMContext *ctx) {
  /*#region*/
  if (!ctx || !ctx->code_segment) return ERR_INVALID_BOUNDARY;

  while (ctx->pc < ctx->header->code_size) {
    uint8_t opcode = ctx->code_segment[ctx->pc++];

    switch (opcode) {
      case OP_NOP:
        break;

      case OP_PUSH_CONST: {
        uint32_t idx = read_uint32_be(ctx);
        if (idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
        if (ctx->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
        ctx->stack[ctx->sp++] = get_constant(ctx, idx);
        break;
      }

      case OP_POP: {
        if (ctx->sp == 0) return ERR_VM_STACK_UNDERFLOW;
        ctx->sp--;
        break;
      }

      case OP_RESOLVE: {
        uint32_t path_idx = read_uint32_be(ctx);
        if (path_idx >= ctx->header->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
        Jsonv_Value path_val = get_constant(ctx, path_idx);
        if (ctx->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
        ctx->stack[ctx->sp++] = path_val;
        break;
      }

      case OP_JUMP: {
        uint32_t target_offset = read_uint32_be(ctx);
        if (target_offset >= ctx->header->code_size) return ERR_VM_INVALID_JUMP;
        ctx->pc = target_offset;
        break;
      }

      case OP_JUMP_IF_FALSE: {
        uint32_t target_offset = read_uint32_be(ctx);
        if (target_offset >= ctx->header->code_size) return ERR_VM_INVALID_JUMP;
        if (ctx->sp == 0) return ERR_VM_STACK_UNDERFLOW;
        Jsonv_Value val = ctx->stack[--ctx->sp];
        bool is_falsy = (val.tag == JSONV_VAL_BOOLEAN && !val.as.boolean) ||
                        (val.tag == JSONV_VAL_NULL) ||
                        (val.tag == JSONV_VAL_UNDEFINED);
        if (is_falsy) {
          ctx->pc = target_offset;
        }
        break;
      }

      case OP_CALL_PROVIDER: {
        uint32_t prov_op_idx = read_uint32_be(ctx);
        (void)prov_op_idx;
        if (ctx->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
        ctx->stack[ctx->sp++] = jsonv_val_bool(true);
        break;
      }

      case OP_CALL_WORKFLOW: {
        uint32_t wf_idx = read_uint32_be(ctx);
        (void)wf_idx;
        if (ctx->call_stack_depth >= VM_CALL_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
        NVMCallFrame *frame = (NVMCallFrame *)na_alloc(ctx->arena, sizeof(NVMCallFrame));
        if (!frame) return ERR_OOM;
        frame->return_pc = ctx->pc;
        frame->prev = ctx->call_stack_top;
        ctx->call_stack_top = frame;
        ctx->call_stack_depth++;
        break;
      }

      case OP_RETURN: {
        if (ctx->call_stack_top) {
          ctx->pc = ctx->call_stack_top->return_pc;
          ctx->call_stack_top = ctx->call_stack_top->prev;
          ctx->call_stack_depth--;
        } else {
          // Top-level workflow completion
          return ERR_SUCCESS;
        }
        break;
      }

      case OP_FORK: {
        if (ctx->pc >= ctx->header->code_size) return ERR_VM_OUT_OF_BOUNDS;
        uint8_t count = ctx->code_segment[ctx->pc++];
        for (uint8_t c = 0; c < count; c++) {
          read_uint32_be(ctx);
        }
        break;
      }

      case OP_JOIN: {
        if (ctx->pc >= ctx->header->code_size) return ERR_VM_OUT_OF_BOUNDS;
        ctx->pc++; // skip strategy byte
        break;
      }

      default:
        return ERR_VM_ILLEGAL_INSTRUCTION;
    }
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

void nvm_close(NVMContext *ctx) {
  /*#region*/
  if (ctx && ctx->mapped_file) {
    munmap((void *)ctx->mapped_file, ctx->mapped_size);
    ctx->mapped_file = NULL;
  }
  /*#endregion*/
}
