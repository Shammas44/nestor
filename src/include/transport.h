#ifndef _NESTOR_TRANSPORT_H
#define _NESTOR_TRANSPORT_H

#include "arena.h"
#include "ast.h"
#include "runner.h"
#include "jsonv/jsonv.h"
#include <stdbool.h>

typedef struct Transport Transport;
typedef struct TransportOps TransportOps;

struct TransportOps {
  int32_t (*start_request)(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StepNode *step, ResponseBuffer *resp_buf, void **handle_out);
  int32_t (*poll_requests)(Transport *t, int *still_running);
  int32_t (*check_completed)(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, void *easy_handle, ResponseBuffer *resp_buf, long *status_code, bool *completed, bool *error);
  void (*cleanup_request)(Transport *t, void *easy_handle);
  void (*destroy)(Transport *t);
};

struct Transport {
  const TransportOps *ops;
  void *impl_data;
};

Transport *transport_curl_new(Arena *arena);
Transport *transport_mock_new(Arena *arena);
void transport_mock_add_response(const char *url, const char *method, long status_code, const char *body);
void transport_mock_clear(void);

#endif
