#ifndef NESTOR_PLUGIN_H
#define NESTOR_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#define NESTOR_API_VERSION_MAJOR 1
#define NESTOR_API_VERSION_MINOR 0

#define NESTOR_LOG_DEBUG 0
#define NESTOR_LOG_INFO  1
#define NESTOR_LOG_WARN  2
#define NESTOR_LOG_ERROR 3

typedef struct {
  void* (*alloc)(void *arena_ptr, size_t size);
  void  (*log)(int level, const char *message);
  const char* (*get_variable)(void *ctx_ptr, const char *json_path);
  int32_t (*set_output)(void *ctx_ptr, const char *key, const char *json_val);
} NestorHostAPI;

typedef struct {
  const char *name;
  const char *version;
  const char *author;
  uint32_t api_major;
  uint32_t api_minor;
} NestorPluginInfo;

typedef struct {
  int32_t (*init)(const NestorHostAPI *host);
  int32_t (*execute)(void *arena_ptr, void *ctx_ptr, const char *args_json);
  void    (*shutdown)(void);
} NestorPluginAPI;

#ifdef _WIN32
  #define NESTOR_EXPORT __declspec(dllexport)
#else
  #define NESTOR_EXPORT __attribute__((visibility("default")))
#endif

NESTOR_EXPORT const NestorPluginInfo* nestor_plugin_query(void);
NESTOR_EXPORT int32_t nestor_plugin_register(const NestorHostAPI *host, NestorPluginAPI *out_api);

#endif // NESTOR_PLUGIN_H
