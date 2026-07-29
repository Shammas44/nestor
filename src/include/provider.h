#ifndef _NESTOR_PROVIDER_H
#define _NESTOR_PROVIDER_H

#include "arena.h"
#include "stringview.h"
#include "error_codes.h"
#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define ERR_PROVIDER_NOT_FOUND -10
#define ERR_PROVIDER_EXEC_FAIL -11
#define ERR_PROVIDER_DLOPEN -12

// Provider SPI function prototype definition
typedef int32_t (*NestorProviderExecFn)(
    Arena *arena,
    const char *operation,
    Jsonv_Value args,
    Jsonv_Value *output,
    char *err_buf,
    size_t err_len
);

typedef struct Provider {
  StringView name;
  StringView library_path;
  void *dl_handle;
  NestorProviderExecFn exec_fn;
  bool is_builtin;
} Provider;

int32_t provider_load(Arena *arena, const char *library_path, Provider *out_provider);
int32_t provider_execute(
    Arena *arena,
    Provider *provider,
    const char *operation,
    Jsonv_Value args,
    Jsonv_Value *output,
    char *err_buf,
    size_t err_len
);
void provider_unload(Provider *provider);

#endif // _NESTOR_PROVIDER_H
