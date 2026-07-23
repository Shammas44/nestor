#include "nestor_plugin.h"
#include <string.h>
#include <stdio.h>

static const NestorHostAPI *host_api = NULL;

static int32_t execute_fn(void *arena_ptr, void *ctx_ptr, const char *args_json) {
  /*#region*/
  (void)arena_ptr;
  (void)args_json;
  
  if (!host_api) return 1;
  
  // Evaluate variable from host
  const char *param_val = host_api->get_variable(ctx_ptr, "inputs.param_in");
  if (!param_val) {
    host_api->log(NESTOR_LOG_ERROR, "Missing inputs.param_in variable!");
    return 1;
  }
  
  // Return computed outputs (properly JSON formatted strings)
  host_api->set_output(ctx_ptr, "status", "\"success\"");
  
  char computed_val[256];
  snprintf(computed_val, sizeof(computed_val), "\"Processed: %s\"", param_val);
  host_api->set_output(ctx_ptr, "computed_val", computed_val);
  
  return 0;
  /*#endregion*/
}

NESTOR_EXPORT const NestorPluginInfo* nestor_plugin_query(void) {
  /*#region*/
  static const NestorPluginInfo info = {
    .name = "Test Dynamic Plugin",
    .version = "1.0.0",
    .author = "Antigravity Team",
    .api_major = NESTOR_API_VERSION_MAJOR,
    .api_minor = NESTOR_API_VERSION_MINOR
  };
  return &info;
  /*#endregion*/
}

NESTOR_EXPORT int32_t nestor_plugin_register(const NestorHostAPI *host, NestorPluginAPI *out_api) {
  /*#region*/
  host_api = host;
  
  out_api->init = NULL;
  out_api->shutdown = NULL;
  out_api->execute = execute_fn;
  
  return 0;
  /*#endregion*/
}
