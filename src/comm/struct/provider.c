#include "provider.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int32_t provider_load(Arena *arena, const char *library_path, Provider *out_provider) {
  /*#region*/
  if (!library_path || !out_provider) return ERR_PROVIDER_DLOPEN;

  void *handle = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    return ERR_PROVIDER_DLOPEN;
  }

  NestorProviderExecFn exec_fn = (NestorProviderExecFn)dlsym(handle, "nestor_provider_execute");
  if (!exec_fn) {
    dlclose(handle);
    return ERR_PROVIDER_DLOPEN;
  }

  size_t path_len = strlen(library_path);
  char *path_copy = (char *)na_alloc(arena, path_len + 1);
  if (path_copy) {
    memcpy(path_copy, library_path, path_len + 1);
  }

  out_provider->name = (StringView){ library_path, path_len };
  out_provider->library_path = (StringView){ path_copy ? path_copy : library_path, path_len };
  out_provider->dl_handle = handle;
  out_provider->exec_fn = exec_fn;
  out_provider->is_builtin = false;

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t provider_execute(
    Arena *arena,
    Provider *provider,
    const char *operation,
    Jsonv_Value args,
    Jsonv_Value *output,
    char *err_buf,
    size_t err_len
) {
  /*#region*/
  if (!provider || !provider->exec_fn) {
    if (err_buf && err_len > 0) {
      snprintf(err_buf, err_len, "Invalid provider execution handle");
    }
    return ERR_PROVIDER_EXEC_FAIL;
  }

  return provider->exec_fn(arena, operation, args, output, err_buf, err_len);
  /*#endregion*/
}

void provider_unload(Provider *provider) {
  /*#region*/
  if (provider && provider->dl_handle) {
    dlclose(provider->dl_handle);
    provider->dl_handle = NULL;
    provider->exec_fn = NULL;
  }
  /*#endregion*/
}
