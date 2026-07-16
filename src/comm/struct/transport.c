#include "transport.h"
#include "evaluator.h"
#include "stringview.h"
#include <curl/curl.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

void *na_alloc(Arena *arena, size_t size);

static size_t my_transport_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  /*#region*/
  size_t realsize = size * nmemb;
  ResponseBuffer *mem = (ResponseBuffer *)userp;
  if (mem->len + realsize >= mem->cap) {
    mem->cap = mem->len + realsize + 4096;
    char *new_buf = na_alloc(mem->arena, mem->cap);
    if (!new_buf) return 0;
    memcpy(new_buf, mem->buf, mem->len);
    mem->buf = new_buf;
  }
  memcpy(mem->buf + mem->len, contents, realsize);
  mem->len += realsize;
  mem->buf[mem->len] = '\0';
  return realsize;
  /*#endregion*/
}

typedef struct {
  void *easy_handle;
  long status_code;
  bool error;
} CompletedRequest;

typedef struct {
  CURLM *multi_handle;
  CompletedRequest completed[64];
  int completed_count;
} CurlTransportData;

static int32_t curl_start_request(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StepNode *step, ResponseBuffer *resp_buf, void **handle_out);
static int32_t curl_poll_requests(Transport *t, int *still_running);
static int32_t curl_check_completed(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, void *easy_handle, ResponseBuffer *resp_buf, long *status_code, bool *completed, bool *error);
static void curl_cleanup_request(Transport *t, void *easy_handle);
static void curl_destroy(Transport *t);

static const TransportOps curl_ops = {
  .start_request = curl_start_request,
  .poll_requests = curl_poll_requests,
  .check_completed = curl_check_completed,
  .cleanup_request = curl_cleanup_request,
  .destroy = curl_destroy
};

Transport *transport_curl_new(Arena *arena) {
  /*#region*/
  Transport *t = na_alloc(arena, sizeof(Transport));
  if (!t) return NULL;
  CurlTransportData *data = na_alloc(arena, sizeof(CurlTransportData));
  if (!data) return NULL;
  data->multi_handle = curl_multi_init();
  if (!data->multi_handle) return NULL;
  data->completed_count = 0;
  t->ops = &curl_ops;
  t->impl_data = data;
  return t;
  /*#endregion*/
}

static int32_t curl_start_request(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StepNode *step, ResponseBuffer *resp_buf, void **handle_out) {
  /*#region*/
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  CURL *curl = curl_easy_init();
  if (!curl) return ERR_HTTP_TRANSPORT;

  StringView resolved_url;
  resolve_string(arena, step->http.url, jsonv_arena, context_val, &resolved_url);
  char *url_cstr = sv_to_cstring(arena, resolved_url);

  curl_easy_setopt(curl, CURLOPT_URL, url_cstr);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  StringView method = step->http.method;
  if (sv_equals_cstr(method, "POST")) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (step->http.body.tag == JSONV_VAL_UNDEFINED) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
  } else if (sv_equals_cstr(method, "PUT")) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    if (step->http.body.tag == JSONV_VAL_UNDEFINED) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
  } else if (sv_equals_cstr(method, "PATCH")) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    if (step->http.body.tag == JSONV_VAL_UNDEFINED) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
  } else if (sv_equals_cstr(method, "DELETE")) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }

  struct curl_slist *header_list = NULL;
  bool has_content_type = false;
  if (step->http.headers.tag == JSONV_VAL_OBJ) {
    Jsonv_Obj *headers_obj = step->http.headers.as.p;
    int len = jsonv_obj_length(headers_obj);
    for (int k = 0; k < len; k++) {
      const char *key = jsonv_obj_key_at(headers_obj, k);
      Jsonv_Value val = jsonv_obj_val_at(headers_obj, k);
      
      StringView resolved_val;
      if (val.tag == JSONV_VAL_STRING) {
        StringView orig_val = { (const char *)val.as.p, jsonv_val_str_len(val) };
        int32_t status = resolve_string(arena, orig_val, jsonv_arena, context_val, &resolved_val);
        if (status != ERR_SUCCESS) {
          curl_easy_cleanup(curl);
          return status;
        }
      } else {
        resolved_val = (StringView){"", 0};
      }

      char *k_str = (char *)key;
      char *v_str = sv_to_cstring(arena, resolved_val);
      size_t req_len = strlen(k_str) + strlen(v_str) + 3;
      char *h = na_alloc(arena, req_len);
      if (h) {
        snprintf(h, req_len, "%s: %s", k_str, v_str);
        header_list = curl_slist_append(header_list, h);
      }
      if (strcasecmp(k_str, "Content-Type") == 0) {
        has_content_type = true;
      }
    }
  }
  if (!has_content_type && step->http.body.tag != JSONV_VAL_UNDEFINED) {
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  if (step->http.body.tag != JSONV_VAL_UNDEFINED) {
    Jsonv_Value resolved_body = resolve_json_value(arena, step->http.body, jsonv_arena, context_val);
    char *body_cstr = "";
    if (resolved_body.tag == JSONV_VAL_STRING) {
      body_cstr = (char *)resolved_body.as.p;
    } else {
      serialize_jsonv_value(arena, resolved_body, &body_cstr);
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_cstr);
  }

  long timeout_ms = 30000;
  if (step->timeout.length > 0) {
    StringView resolved_timeout;
    resolve_string(arena, step->timeout, jsonv_arena, context_val, &resolved_timeout);
    timeout_ms = parse_duration_ms(resolved_timeout);
  }
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

  resp_buf->arena = arena;
  resp_buf->len = 0;
  resp_buf->cap = 4096;
  resp_buf->buf = na_alloc(arena, resp_buf->cap);
  if (!resp_buf->buf) {
    curl_easy_cleanup(curl);
    return ERR_OOM;
  }
  resp_buf->buf[0] = '\0';
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_transport_curl_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)resp_buf);

  curl_easy_setopt(curl, CURLOPT_PRIVATE, (void *)header_list);

  curl_multi_add_handle(data->multi_handle, curl);
  *handle_out = curl;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t curl_poll_requests(Transport *t, int *still_running) {
  /*#region*/
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  CURLMcode mc = curl_multi_perform(data->multi_handle, still_running);
  if (mc != CURLM_OK) return ERR_HTTP_TRANSPORT;

  int msgs_left;
  CURLMsg *msg;
  while ((msg = curl_multi_info_read(data->multi_handle, &msgs_left))) {
    if (msg->msg == CURLMSG_DONE) {
      if (data->completed_count < 64) {
        CompletedRequest *cr = &data->completed[data->completed_count++];
        cr->easy_handle = msg->easy_handle;
        long status_code = 0;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status_code);
        cr->status_code = status_code;
        cr->error = (msg->data.result != CURLE_OK);
      }
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t curl_check_completed(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, void *easy_handle, ResponseBuffer *resp_buf, long *status_code, bool *completed, bool *error) {
  /*#region*/
  (void)arena;
  (void)jsonv_arena;
  (void)resp_buf;
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  *completed = false;
  *error = false;

  for (int i = 0; i < data->completed_count; i++) {
    if (data->completed[i].easy_handle == easy_handle) {
      *status_code = data->completed[i].status_code;
      *error = data->completed[i].error;
      *completed = true;

      // Remove from completed array by shifting
      for (int j = i; j < data->completed_count - 1; j++) {
        data->completed[j] = data->completed[j + 1];
      }
      data->completed_count--;
      break;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static void curl_cleanup_request(Transport *t, void *easy_handle) {
  /*#region*/
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  if (easy_handle) {
    struct curl_slist *header_list = NULL;
    curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &header_list);
    if (header_list) {
      curl_slist_free_all(header_list);
    }
    curl_multi_remove_handle(data->multi_handle, easy_handle);
    curl_easy_cleanup(easy_handle);
  }
  /*#endregion*/
}

static void curl_destroy(Transport *t) {
  /*#region*/
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  if (data->multi_handle) {
    curl_multi_cleanup(data->multi_handle);
  }
  /*#endregion*/
}


/* Mock Transport Implementation */

typedef struct MockRequestState MockRequestState;
struct MockRequestState {
  long status_code;
  const char *body;
  struct timeval start_time;
  long delay_ms;
};

typedef struct MockResponseEntry MockResponseEntry;
struct MockResponseEntry {
  char url[256];
  char method[16];
  long status_code;
  char body[4096];
  MockResponseEntry *next;
};

static MockResponseEntry *mock_responses_head = NULL;

void transport_mock_add_response(const char *url, const char *method, long status_code, const char *body) {
  /*#region*/
  // Note: For unit testing, since malloc is forbidden, we can statically/arena allocate it.
  // In our tests, we will just allocate a static pool or allow arena allocation if we pass it,
  // but to keep it simple, we can just use a static array of mock entries!
  static MockResponseEntry static_pool[64];
  static size_t static_pool_idx = 0;
  
  MockResponseEntry *entry = NULL;
  if (static_pool_idx < 64) {
    entry = &static_pool[static_pool_idx++];
  } else {
    return;
  }
  strncpy(entry->url, url, sizeof(entry->url) - 1);
  strncpy(entry->method, method, sizeof(entry->method) - 1);
  entry->status_code = status_code;
  strncpy(entry->body, body, sizeof(entry->body) - 1);
  entry->next = mock_responses_head;
  mock_responses_head = entry;
  /*#endregion*/
}

static int32_t mock_start_request(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StepNode *step, ResponseBuffer *resp_buf, void **handle_out) {
  /*#region*/
  (void)t;
  StringView resolved_url;
  resolve_string(arena, step->http.url, jsonv_arena, context_val, &resolved_url);
  char *url_cstr = sv_to_cstring(arena, resolved_url);

  MockResponseEntry *curr = mock_responses_head;
  MockResponseEntry *match = NULL;
  while (curr) {
    if (strstr(url_cstr, curr->url) && sv_equals_cstr(step->http.method, curr->method)) {
      match = curr;
      break;
    }
    curr = curr->next;
  }

  MockRequestState *state = na_alloc(arena, sizeof(MockRequestState));
  if (!state) return ERR_OOM;

  if (match) {
    state->status_code = match->status_code;
    state->body = match->body;
  } else {
    state->status_code = 404;
    state->body = "{\"error\": \"not found\"}";
  }

  resp_buf->arena = arena;
  size_t body_len = strlen(state->body);
  resp_buf->buf = na_alloc(arena, body_len + 1);
  if (!resp_buf->buf) return ERR_OOM;
  memcpy(resp_buf->buf, state->body, body_len);
  resp_buf->buf[body_len] = '\0';
  resp_buf->len = body_len;
  resp_buf->cap = body_len + 1;

  gettimeofday(&state->start_time, NULL);
  state->delay_ms = 0;
  if (strstr(url_cstr, "/delay/")) {
    char *delay_ptr = strstr(url_cstr, "/delay/") + 7;
    double delay_sec = atof(delay_ptr);
    state->delay_ms = (long)(delay_sec * 1000.0);
  }

  *handle_out = (void *)state;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t mock_poll_requests(Transport *t, int *still_running) {
  /*#region*/
  (void)t;
  *still_running = 0;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t mock_check_completed(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, void *easy_handle, ResponseBuffer *resp_buf, long *status_code, bool *completed, bool *error) {
  /*#region*/
  (void)t;
  (void)arena;
  (void)jsonv_arena;
  (void)resp_buf;
  MockRequestState *state = (MockRequestState *)easy_handle;
  *status_code = state->status_code;
  *error = false;

  struct timeval now;
  gettimeofday(&now, NULL);
  long elapsed_ms = (now.tv_sec - state->start_time.tv_sec) * 1000 + 
                    (now.tv_usec - state->start_time.tv_usec) / 1000;
  if (elapsed_ms >= state->delay_ms) {
    *completed = true;
  } else {
    *completed = false;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static void mock_cleanup_request(Transport *t, void *easy_handle) {
  /*#region*/
  (void)t;
  (void)easy_handle;
  /*#endregion*/
}

static void mock_destroy(Transport *t) {
  /*#region*/
  (void)t;
  /*#endregion*/
}

static const TransportOps mock_ops = {
  .start_request = mock_start_request,
  .poll_requests = mock_poll_requests,
  .check_completed = mock_check_completed,
  .cleanup_request = mock_cleanup_request,
  .destroy = mock_destroy
};

Transport *transport_mock_new(Arena *arena) {
  /*#region*/
  Transport *t = na_alloc(arena, sizeof(Transport));
  if (!t) return NULL;
  t->ops = &mock_ops;
  t->impl_data = NULL;
  return t;
  /*#endregion*/
}
