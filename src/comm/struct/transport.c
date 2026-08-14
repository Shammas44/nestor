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
#include <fcntl.h>

void *na_alloc(Arena *arena, size_t size);

typedef struct {
  struct curl_slist *req_headers;
  char cache_control[256];
  char expires[128];
  char etag[128];
  char last_modified[128];
  Arena *arena;
  ResponseHeaderNode *headers_head;
} CurlRequestState;

static size_t my_header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
  /*#region*/
  size_t total = size * nitems;
  CurlRequestState *state = (CurlRequestState *)userdata;
  if (!state) return total;

  char line[512];
  size_t copy_len = total < sizeof(line) - 1 ? total : sizeof(line) - 1;
  memcpy(line, buffer, copy_len);
  line[copy_len] = '\0';

  char *colon = strchr(line, ':');
  if (colon) {
    *colon = '\0';
    char *name = line;
    char *value = colon + 1;
    while (*name == ' ' || *name == '\t') name++;
    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    char *end = name + strlen(name) - 1;
    while (end >= name && (*end == ' ' || *end == '\t')) *end-- = '\0';
    end = value + strlen(value) - 1;
    while (end >= value && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';

    if (strcasecmp(name, "Cache-Control") == 0) {
      strncpy(state->cache_control, value, sizeof(state->cache_control) - 1);
    } else if (strcasecmp(name, "Expires") == 0) {
      strncpy(state->expires, value, sizeof(state->expires) - 1);
    } else if (strcasecmp(name, "ETag") == 0) {
      strncpy(state->etag, value, sizeof(state->etag) - 1);
    } else if (strcasecmp(name, "Last-Modified") == 0) {
      strncpy(state->last_modified, value, sizeof(state->last_modified) - 1);
    }

    if (state->arena) {
      ResponseHeaderNode *node = na_alloc(state->arena, sizeof(ResponseHeaderNode));
      if (node) {
        node->name = na_alloc(state->arena, strlen(name) + 1);
        if (node->name) strcpy(node->name, name);
        node->value = na_alloc(state->arena, strlen(value) + 1);
        if (node->value) strcpy(node->value, value);
        node->next = state->headers_head;
        state->headers_head = node;
      }
    }
  }
  return total;
  /*#endregion*/
}

static size_t my_transport_curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  /*#region*/
  size_t realsize = size * nmemb;
  ResponseBuffer *mem = (ResponseBuffer *)userp;
  if (mem->is_stream) {
    // If the step is configured to stream, we write directly to the file descriptor
    // rather than accumulating in memory. This achieves constant O(1) RAM scaling.
    if (mem->stream_fd >= 0) {
      ssize_t written = write(mem->stream_fd, contents, realsize);
      if (written < 0) {
        return 0; // abort CURL transfer on write error
      }
      mem->len += (size_t)written;
    }
    return realsize;
  }
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

  CurlRequestState *req_state = na_alloc(arena, sizeof(CurlRequestState));
  if (!req_state) {
    curl_easy_cleanup(curl);
    return ERR_OOM;
  }
  memset(req_state, 0, sizeof(CurlRequestState));
  req_state->arena = arena;
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, my_header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)req_state);

  StringView resolved_url = {NULL, 0};
  int32_t status = resolve_string(arena, step->http.url, jsonv_arena, context_val, &resolved_url);
  if (status != ERR_SUCCESS) return status;
  char *url_cstr = sv_to_cstring(arena, resolved_url);
  curl_easy_setopt(curl, CURLOPT_URL, url_cstr);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  if (step->http.insecure) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }

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

  Jsonv_Value cache_etag_val;
  if (jsonv_obj_get(context_val.as.p, "_cache_etag", &cache_etag_val) && cache_etag_val.tag == JSONV_VAL_STRING) {
    char *etag_str = (char *)cache_etag_val.as.p;
    size_t req_len = strlen("If-None-Match: ") + strlen(etag_str) + 1;
    char *h = na_alloc(arena, req_len);
    if (h) {
      snprintf(h, req_len, "If-None-Match: %s", etag_str);
      header_list = curl_slist_append(header_list, h);
    }
  }
  Jsonv_Value cache_lm_val;
  if (jsonv_obj_get(context_val.as.p, "_cache_last_modified", &cache_lm_val) && cache_lm_val.tag == JSONV_VAL_STRING) {
    char *lm_str = (char *)cache_lm_val.as.p;
    size_t req_len = strlen("If-Modified-Since: ") + strlen(lm_str) + 1;
    char *h = na_alloc(arena, req_len);
    if (h) {
      snprintf(h, req_len, "If-Modified-Since: %s", lm_str);
      header_list = curl_slist_append(header_list, h);
    }
  }

  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }
  req_state->req_headers = header_list;

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
    StringView resolved_timeout = {NULL, 0};
    int32_t status = resolve_string(arena, step->timeout, jsonv_arena, context_val, &resolved_timeout);
    if (status != ERR_SUCCESS) return status;
    timeout_ms = parse_duration_ms(resolved_timeout);
  }
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);

  resp_buf->arena = arena;
  resp_buf->len = 0;
  resp_buf->headers_head = NULL;
  resp_buf->is_stream = step->http.stream;
  if (resp_buf->is_stream) {
    resp_buf->cap = 0;
    resp_buf->buf = NULL;
    // Generate a unique stream file path for this job step
    snprintf(resp_buf->stream_file_path, sizeof(resp_buf->stream_file_path), ".nestor_stream_%.*s.json", (int)step->id.length, step->id.data ? step->id.data : "default");
    resp_buf->stream_fd = open(resp_buf->stream_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (resp_buf->stream_fd < 0) {
      curl_easy_cleanup(curl);
      return ERR_HTTP_TRANSPORT;
    }
  } else {
    resp_buf->stream_fd = -1;
    resp_buf->cap = 4096;
    resp_buf->buf = na_alloc(arena, resp_buf->cap);
    if (!resp_buf->buf) {
      curl_easy_cleanup(curl);
      return ERR_OOM;
    }
    resp_buf->buf[0] = '\0';
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_transport_curl_write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)resp_buf);

  curl_easy_setopt(curl, CURLOPT_PRIVATE, (void *)req_state);

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
        if (msg->data.result != CURLE_OK) {
          fprintf(stderr, "DEBUG: curl_multi completed transfer error: %s (%d)\n", curl_easy_strerror(msg->data.result), msg->data.result);
        }
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
  CurlTransportData *data = (CurlTransportData *)t->impl_data;
  *completed = false;
  *error = false;

  for (int i = 0; i < data->completed_count; i++) {
    if (data->completed[i].easy_handle == easy_handle) {
      *status_code = data->completed[i].status_code;
      *error = data->completed[i].error;
      *completed = true;

      CurlRequestState *req_state = NULL;
      curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &req_state);
      if (req_state && resp_buf) {
        strncpy(resp_buf->cache_control, req_state->cache_control, sizeof(resp_buf->cache_control) - 1);
        resp_buf->cache_control[sizeof(resp_buf->cache_control) - 1] = '\0';
        strncpy(resp_buf->expires, req_state->expires, sizeof(resp_buf->expires) - 1);
        resp_buf->expires[sizeof(resp_buf->expires) - 1] = '\0';
        strncpy(resp_buf->etag, req_state->etag, sizeof(resp_buf->etag) - 1);
        resp_buf->etag[sizeof(resp_buf->etag) - 1] = '\0';
        strncpy(resp_buf->last_modified, req_state->last_modified, sizeof(resp_buf->last_modified) - 1);
        resp_buf->last_modified[sizeof(resp_buf->last_modified) - 1] = '\0';
        resp_buf->headers_head = req_state->headers_head;
      }

      if (resp_buf && resp_buf->is_stream && resp_buf->stream_fd >= 0) {
        close(resp_buf->stream_fd);
        resp_buf->stream_fd = -1;
      }

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
    CurlRequestState *req_state = NULL;
    curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &req_state);
    if (req_state && req_state->req_headers) {
      curl_slist_free_all(req_state->req_headers);
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

typedef struct MockHeaderNode MockHeaderNode;
struct MockHeaderNode {
  char key[128];
  char value[256];
  MockHeaderNode *next;
};

typedef struct MockResponseEntry MockResponseEntry;
struct MockResponseEntry {
  char url[256];
  char method[16];
  long status_code;
  char body[4096];
  char cache_control[256];
  char etag[128];
  char last_modified[128];
  MockHeaderNode *headers_head;
  MockResponseEntry *next;
};

static MockResponseEntry *mock_responses_head = NULL;
static MockResponseEntry static_pool[128];
static size_t static_pool_idx = 0;

static MockHeaderNode static_header_pool[512];
static size_t static_header_pool_idx = 0;

void transport_mock_clear(void) {
  /*#region*/
  mock_responses_head = NULL;
  static_pool_idx = 0;
  static_header_pool_idx = 0;
  /*#endregion*/
}

void transport_mock_add_response(const char *url, const char *method, long status_code, const char *body) {
  /*#region*/
  MockResponseEntry *entry = NULL;
  if (static_pool_idx < 128) {
    entry = &static_pool[static_pool_idx++];
  } else {
    return;
  }
  memset(entry, 0, sizeof(MockResponseEntry));
  strncpy(entry->url, url, sizeof(entry->url) - 1);
  strncpy(entry->method, method, sizeof(entry->method) - 1);
  entry->status_code = status_code;
  strncpy(entry->body, body, sizeof(entry->body) - 1);
  entry->next = mock_responses_head;
  mock_responses_head = entry;
  /*#endregion*/
}

void transport_mock_add_header(const char *url, const char *method, const char *key, const char *value) {
  /*#region*/
  MockResponseEntry *curr = mock_responses_head;
  while (curr) {
    if (strstr(url, curr->url) && strcmp(curr->method, method) == 0) {
      if (strcasecmp(key, "Cache-Control") == 0) {
        strncpy(curr->cache_control, value, sizeof(curr->cache_control) - 1);
        curr->cache_control[sizeof(curr->cache_control) - 1] = '\0';
      } else if (strcasecmp(key, "ETag") == 0) {
        strncpy(curr->etag, value, sizeof(curr->etag) - 1);
        curr->etag[sizeof(curr->etag) - 1] = '\0';
      } else if (strcasecmp(key, "Last-Modified") == 0) {
        strncpy(curr->last_modified, value, sizeof(curr->last_modified) - 1);
        curr->last_modified[sizeof(curr->last_modified) - 1] = '\0';
      }

      if (static_header_pool_idx < 512) {
        MockHeaderNode *node = &static_header_pool[static_header_pool_idx++];
        strncpy(node->key, key, sizeof(node->key) - 1);
        node->key[sizeof(node->key) - 1] = '\0';
        strncpy(node->value, value, sizeof(node->value) - 1);
        node->value[sizeof(node->value) - 1] = '\0';
        node->next = curr->headers_head;
        curr->headers_head = node;
      }
      break;
    }
    curr = curr->next;
  }
  /*#endregion*/
}

static int32_t mock_start_request(Transport *t, Arena *arena, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StepNode *step, ResponseBuffer *resp_buf, void **handle_out) {
  /*#region*/
  (void)t;
  StringView resolved_url = {NULL, 0};
  int32_t status = resolve_string(arena, step->http.url, jsonv_arena, context_val, &resolved_url);
  if (status != ERR_SUCCESS) return status;
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
    strncpy(resp_buf->cache_control, match->cache_control, sizeof(resp_buf->cache_control) - 1);
    resp_buf->cache_control[sizeof(resp_buf->cache_control) - 1] = '\0';
    strncpy(resp_buf->etag, match->etag, sizeof(resp_buf->etag) - 1);
    resp_buf->etag[sizeof(resp_buf->etag) - 1] = '\0';
    strncpy(resp_buf->last_modified, match->last_modified, sizeof(resp_buf->last_modified) - 1);
    resp_buf->last_modified[sizeof(resp_buf->last_modified) - 1] = '\0';
    resp_buf->headers_head = NULL;

    MockHeaderNode *hcurr = match->headers_head;
    while (hcurr) {
      ResponseHeaderNode *node = na_alloc(arena, sizeof(ResponseHeaderNode));
      if (node) {
        node->name = na_alloc(arena, strlen(hcurr->key) + 1);
        if (node->name) strcpy(node->name, hcurr->key);
        node->value = na_alloc(arena, strlen(hcurr->value) + 1);
        if (node->value) strcpy(node->value, hcurr->value);
        node->next = resp_buf->headers_head;
        resp_buf->headers_head = node;
      }
      hcurr = hcurr->next;
    }
  } else {
    state->status_code = 404;
    state->body = "{\"error\": \"not found\"}";
  }

  resp_buf->arena = arena;
  resp_buf->is_stream = step->http.stream;
  size_t body_len = strlen(state->body);
  if (resp_buf->is_stream) {
    resp_buf->cap = 0;
    resp_buf->buf = NULL;
    resp_buf->len = body_len;
    snprintf(resp_buf->stream_file_path, sizeof(resp_buf->stream_file_path), ".nestor_stream_%.*s.json", (int)step->id.length, step->id.data ? step->id.data : "default");
    resp_buf->stream_fd = open(resp_buf->stream_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (resp_buf->stream_fd < 0) {
      return ERR_HTTP_TRANSPORT;
    }
    ssize_t written = write(resp_buf->stream_fd, state->body, body_len);
    (void)written;
  } else {
    resp_buf->stream_fd = -1;
    resp_buf->buf = na_alloc(arena, body_len + 1);
    if (!resp_buf->buf) return ERR_OOM;
    memcpy(resp_buf->buf, state->body, body_len);
    resp_buf->buf[body_len] = '\0';
    resp_buf->len = body_len;
    resp_buf->cap = body_len + 1;
  }

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
    if (resp_buf && resp_buf->is_stream && resp_buf->stream_fd >= 0) {
      close(resp_buf->stream_fd);
      resp_buf->stream_fd = -1;
    }
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
