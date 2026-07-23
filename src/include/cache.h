#ifndef _NESTOR_CACHE_H
#define _NESTOR_CACHE_H

#include "arena.h"
#include <stdint.h>
#include <stddef.h>

int32_t cache_init(const char *db_path, int max_entries);
void cache_close(void);

void cache_generate_key(const char *job_type, const char *spec_json, const char *inputs_json, const char *env_json, char *out_hex);

int32_t cache_lookup(Arena *arena, const char *key, long *status_code_out, char **headers_json_out, char **output_payload_out, char **etag_out, char **last_modified_out);

int32_t cache_store(const char *key, const char *job_id, const char *job_type, long status_code, const char *headers_json, const char *output_payload, const char *etag, const char *last_modified, int ttl_seconds);

#endif
