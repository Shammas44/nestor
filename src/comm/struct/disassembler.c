#include "arena.h"
#include "bytecode.h"
#include "crypto.h"
#include "error_codes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void *na_alloc(Arena *arena, size_t size);

static void get_active_public_key(uint8_t *pub_key) {
  /*#region*/
  const char *env_pub = getenv("NESTOR_PUBLIC_KEY");
  if (env_pub && strlen(env_pub) == 64) {
    for (int i = 0; i < 32; i++) {
      unsigned int byte_val;
      sscanf(env_pub + i * 2, "%02x", &byte_val);
      pub_key[i] = (uint8_t)byte_val;
    }
  } else {
    // Default fallback public key
    const uint8_t DEFAULT_PUB_KEY[32] = {
      0x01, 0x46, 0x1D, 0x96, 0x63, 0x74, 0xAA, 0x56, 0x82, 0xDF, 0x58, 0xEC, 0xE3, 0x72, 0x76, 0xB5, 0x9F, 0x76, 0x4C, 0x5E, 0x94, 0xE9, 0xB6, 0x65, 0xBE, 0x9F, 0xD4, 0xBE, 0xE4, 0xAF, 0x1A, 0x01
    };
    memcpy(pub_key, DEFAULT_PUB_KEY, 32);
  }
  /*#endregion*/
}

static const char *get_constant_str(const uint8_t *const_pool, uint32_t const_pool_count, uint32_t idx, Arena *arena) {
  /*#region*/
  if (idx >= const_pool_count) return "<out of bounds>";

  uint32_t offset = 0;
  for (uint32_t i = 0; i < idx; i++) {
    uint32_t len = (const_pool[offset] << 24) |
                   (const_pool[offset + 1] << 16) |
                   (const_pool[offset + 2] << 8) |
                   (const_pool[offset + 3]);
    offset += 4 + len;
  }

  uint32_t len = (const_pool[offset] << 24) |
                 (const_pool[offset + 1] << 16) |
                 (const_pool[offset + 2] << 8) |
                 (const_pool[offset + 3]);
  const char *str_data = (const char *)(const_pool + offset + 4);

  char *buf = (char *)na_alloc(arena, len + 1);
  if (!buf) return "<oom>";
  memcpy(buf, str_data, len);
  buf[len] = '\0';
  return buf;
  /*#endregion*/
}

int32_t disassemble_nbc_file(Arena *arena, const char *nbc_file_path) {
  /*#region*/
  if (!arena || !nbc_file_path) return ERR_INVALID_BOUNDARY;

  int fd = open(nbc_file_path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Error: could not open file '%s'\n", nbc_file_path);
    return ERR_HTTP_TRANSPORT;
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(NVMHeader)) {
    fprintf(stderr, "Error: invalid file size\n");
    close(fd);
    return ERR_INVALID_BOUNDARY;
  }

  size_t mapped_size = (size_t)st.st_size;
  const uint8_t *mapped_file = (const uint8_t *)mmap(NULL, mapped_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);

  if (mapped_file == MAP_FAILED) {
    fprintf(stderr, "Error: failed to map file\n");
    return ERR_HTTP_TRANSPORT;
  }

  const NVMHeader *header = (const NVMHeader *)mapped_file;
  if (memcmp(header->magic, "NEST", 4) != 0) {
    fprintf(stderr, "Error: invalid NEST magic header\n");
    munmap((void *)mapped_file, mapped_size);
    return ERR_INVALID_BOUNDARY;
  }

  const char *sig_status = "Unsigned";
  if (header->flags & 1) {
    uint8_t *verify_buf = na_alloc(arena, mapped_size);
    if (!verify_buf) {
      fprintf(stderr, "Error: memory allocation failure during verification\n");
      munmap((void *)mapped_file, mapped_size);
      return ERR_OOM;
    }
    memcpy(verify_buf, mapped_file, mapped_size);
    memset(((NVMHeader *)verify_buf)->signature, 0, 64);

    uint8_t active_pub_key[32];
    get_active_public_key(active_pub_key);

    int32_t verify_res = crypto_verify_binary(active_pub_key, verify_buf, mapped_size, header->signature);
    if (verify_res == 0) {
      sig_status = "Valid (Ed25519)";
    } else {
      sig_status = "Invalid/Verification Failed";
    }
  }

  const uint8_t *const_pool = mapped_file + header->const_pool_offset;
  const uint8_t *code_segment = mapped_file + header->code_offset;
  uint32_t code_size = header->code_size;

  // Print workflow metadata
  const char *wf_name = "unknown";
  if (header->wf_table_count > 0 && header->wf_table_offset + sizeof(NVMWorkflowEntry) <= mapped_size) {
    const NVMWorkflowEntry *entry = (const NVMWorkflowEntry *)(mapped_file + header->wf_table_offset);
    wf_name = get_constant_str(const_pool, header->const_pool_count, entry->name_const_idx, arena);
  }

  printf("; Source: %s\n", wf_name);
  printf("; Signature: %s\n\n", sig_status);

  // Dump Constant Pool
  printf(".const_pool:\n");
  uint32_t offset = 0;
  for (uint32_t i = 0; i < header->const_pool_count; i++) {
    uint32_t len = (const_pool[offset] << 24) |
                   (const_pool[offset + 1] << 16) |
                   (const_pool[offset + 2] << 8) |
                   (const_pool[offset + 3]);
    const char *str_data = (const char *)(const_pool + offset + 4);
    printf("  [%u] \"%.*s\"\n", i, (int)len, str_data);
    offset += 4 + len;
  }
  printf("\n");

  // Disassemble instructions
  printf(".code:\n");
  uint32_t pc = 0;
  while (pc < code_size) {
    uint8_t op = code_segment[pc];
    printf("  0x%04X: ", pc);

    switch (op) {
      case OP_NOP:
        printf("OP_NOP\n");
        pc += 1;
        break;

      case OP_PUSH_CONST: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_PUSH_CONST    %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_POP:
        printf("OP_POP\n");
        pc += 1;
        break;

      case OP_RESOLVE: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_RESOLVE       %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_JUMP: {
        uint32_t target = (code_segment[pc + 1] << 24) |
                          (code_segment[pc + 2] << 16) |
                          (code_segment[pc + 3] << 8) |
                          (code_segment[pc + 4]);
        printf("OP_JUMP          0x%04X\n", target);
        pc += 5;
        break;
      }

      case OP_JUMP_IF_FALSE: {
        uint32_t target = (code_segment[pc + 1] << 24) |
                          (code_segment[pc + 2] << 16) |
                          (code_segment[pc + 3] << 8) |
                          (code_segment[pc + 4]);
        printf("OP_JUMP_IF_FALSE 0x%04X\n", target);
        pc += 5;
        break;
      }

      case OP_CALL_PROVIDER: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_CALL_PROVIDER %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_CALL_WORKFLOW: {
        uint32_t target = (code_segment[pc + 1] << 24) |
                          (code_segment[pc + 2] << 16) |
                          (code_segment[pc + 3] << 8) |
                          (code_segment[pc + 4]);
        printf("OP_CALL_WORKFLOW 0x%04X\n", target);
        pc += 5;
        break;
      }

      case OP_RETURN:
        printf("OP_RETURN\n");
        pc += 1;
        break;

      case OP_FORK: {
        uint8_t count = code_segment[pc + 1];
        printf("OP_FORK          %u", count);
        for (uint8_t c = 0; c < count; c++) {
          uint32_t target = (code_segment[pc + 2 + c * 4] << 24) |
                            (code_segment[pc + 3 + c * 4] << 16) |
                            (code_segment[pc + 4 + c * 4] << 8) |
                            (code_segment[pc + 5 + c * 4]);
          printf(", 0x%04X", target);
        }
        printf("\n");
        pc += 2 + count * 4;
        break;
      }

      case OP_JOIN: {
        uint8_t strat = code_segment[pc + 1];
        uint32_t idx = (code_segment[pc + 2] << 24) |
                       (code_segment[pc + 3] << 16) |
                       (code_segment[pc + 4] << 8) |
                       (code_segment[pc + 5]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        const char *strat_str = "all";
        if (strat == 2) strat_str = "any";
        else if (strat == 3) strat_str = "n_required";
        printf("OP_JOIN          %-10u ; strategy=%s, job=\"%s\"\n", idx, strat_str, val);
        pc += 6;
        break;
      }

      case OP_LOAD_ENV: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_LOAD_ENV      %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_STORE_VAR: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_STORE_VAR     %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_COMPLETE_JOB: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_COMPLETE_JOB  %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      case OP_SKIP_JOB: {
        uint32_t idx = (code_segment[pc + 1] << 24) |
                       (code_segment[pc + 2] << 16) |
                       (code_segment[pc + 3] << 8) |
                       (code_segment[pc + 4]);
        const char *val = get_constant_str(const_pool, header->const_pool_count, idx, arena);
        printf("OP_SKIP_JOB      %-10u ; \"%s\"\n", idx, val);
        pc += 5;
        break;
      }

      default:
        printf("OP_UNKNOWN       0x%02X\n", op);
        pc += 1;
        break;
    }
  }

  munmap((void *)mapped_file, mapped_size);
  return ERR_SUCCESS;
  /*#endregion*/
}
