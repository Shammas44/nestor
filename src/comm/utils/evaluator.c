#include "evaluator.h"
#include "stringview.h"
#include <jsonata/jsonata.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct {
  const char *path;
  Jsonata_Arena *jsonata_arena;
} NestorLazyContext;

typedef struct {
  Jsonata_Value *result;
  Jsonata_Arena *arena;
} LazyMatchContext;

static void nestor_lazy_match_cb(void *user_data, Jsonata_ValType type, const char *val, size_t val_len) {
  /*#region*/
  LazyMatchContext *ctx = (LazyMatchContext *)user_data;
  Jsonata_Value *res = jsonata_value_alloc(ctx->arena);
  if (!res) return;
  res->type = type;
  if (type == JSONATA_VAL_STRING) {
    char *str_buf = (jsonata_arena_alloc)(ctx->arena, val_len + 1);
    if (str_buf) {
      memcpy(str_buf, val, val_len);
      str_buf[val_len] = '\0';
      res->u.s.ptr = str_buf;
      res->u.s.len = val_len;
    }
  } else if (type == JSONATA_VAL_NUMBER) {
    char *tmp = (jsonata_arena_alloc)(ctx->arena, val_len + 1);
    if (tmp) {
      memcpy(tmp, val, val_len);
      tmp[val_len] = '\0';
      res->u.n = atof(tmp);
    }
  } else if (type == JSONATA_VAL_BOOL) {
    res->u.b = (val_len == 4 && strncmp(val, "true", 4) == 0);
  }
  ctx->result = res;
  /*#endregion*/
}

static Jsonata_Value *nestor_lazy_get_property(void *context, const char *key, size_t key_len) {
  /*#region*/
  NestorLazyContext *lazy_ctx = (NestorLazyContext *)context;
  int fd = open(lazy_ctx->path, O_RDONLY);
  if (fd < 0) return NULL;

  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size == 0) {
    close(fd);
    return NULL;
  }

  void *addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  char *key_cstr = (char *)jsonata_arena_alloc(lazy_ctx->jsonata_arena, key_len + 1);
  if (!key_cstr) {
    munmap(addr, st.st_size);
    close(fd);
    return NULL;
  }
  memcpy(key_cstr, key, key_len);
  key_cstr[key_len] = '\0';

  LazyMatchContext match_ctx = { .result = NULL, .arena = lazy_ctx->jsonata_arena };

  Jsonata_StreamFilter *filter = jsonata_stream_filter_create(key_cstr, nestor_lazy_match_cb, &match_ctx, lazy_ctx->jsonata_arena);
  if (!filter) {
    munmap(addr, st.st_size);
    close(fd);
    return NULL;
  }

  jsonv_sax_callbacks callbacks = {
    .on_begin_object = jsonata_stream_filter_on_begin_object,
    .on_end_object = jsonata_stream_filter_on_end_object,
    .on_begin_array = jsonata_stream_filter_on_begin_array,
    .on_end_array = jsonata_stream_filter_on_end_array,
    .on_object_key = jsonata_stream_filter_on_object_key,
    .on_string = jsonata_stream_filter_on_string,
    .on_number = jsonata_stream_filter_on_number,
    .on_boolean = jsonata_stream_filter_on_boolean,
    .on_null = jsonata_stream_filter_on_null
  };

  jsonv_parse_sax((const unsigned char *)addr, st.st_size, &callbacks, filter);

  munmap(addr, st.st_size);
  close(fd);

  return match_ctx.result;
  /*#endregion*/
}

void *na_alloc(Arena *arena, size_t size);

static void *my_jsonata_arena_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void my_jsonata_arena_reset(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static void my_jsonata_arena_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  (void)user_data;
  (void)keep_size;
  /*#endregion*/
}

static void my_jsonata_arena_destroy(void *user_data) {
  /*#region*/
  (void)user_data;
  /*#endregion*/
}

static const Jsonata_Arena_Ops my_jsonata_ops = {
  .alloc = my_jsonata_arena_alloc,
  .reset = my_jsonata_arena_reset,
  .reset_to = my_jsonata_arena_reset_to,
  .destroy = my_jsonata_arena_destroy
};

// Converts a Jsonv_Value (from the jsonv library) recursively into a Jsonata_Value
// suitable for evaluation by the jsonata expression engine.
static Jsonata_Value *convert_jsonv_to_jsonata_impl(Jsonata_Arena *jsonata_arena, Jsonv_Value v, int depth) {
  /*#region*/
  if (depth > 64) {
    fprintf(stderr, "CYCLE DETECTED: depth=%d, tag=%d\n", depth, v.tag);
    if (v.tag == JSONV_VAL_OBJ) {
      Jsonv_Obj *obj = v.as.p;
      int len = jsonv_obj_length(obj);
      fprintf(stderr, "Circular Object keys (%d):\n", len);
      for (int i = 0; i < len; i++) {
        fprintf(stderr, "  [%d] key='%s'\n", i, jsonv_obj_key_at(obj, i));
      }
    }
    return NULL;
  }

  Jsonata_Value *res = jsonata_value_alloc(jsonata_arena);
  if (!res)
    return NULL;

  switch (v.tag) {
    case JSONV_VAL_UNDEFINED:
      res->type = JSONATA_VAL_UNDEFINED;
      break;
    case JSONV_VAL_NULL:
      res->type = JSONATA_VAL_NULL;
      break;
    case JSONV_VAL_BOOLEAN:
      res->type = JSONATA_VAL_BOOL;
      res->u.b = v.as.boolean;
      break;
    case JSONV_VAL_INT:
      res->type = JSONATA_VAL_NUMBER;
      res->u.n = (double)v.as.i;
      break;
    case JSONV_VAL_DOUBLE:
      res->type = JSONATA_VAL_NUMBER;
      res->u.n = v.as.d;
      break;
    case JSONV_VAL_STRING:
      res->type = JSONATA_VAL_STRING;
      res->u.s.ptr = (const char *)v.as.p;
      res->u.s.len = jsonv_val_str_len(v);
      break;
    case JSONV_VAL_ARRAY: {
      res->type = JSONATA_VAL_ARRAY;
      Jsonv_Arr *arr = v.as.p;
      int len = jsonv_arr_length(arr);
      res->u.array.count = len;
      res->u.array.cons = false;
      if (len > 0) {
        res->u.array.items = jsonata_arena_alloc(jsonata_arena, len * sizeof(Jsonata_Value *));
        if (!res->u.array.items)
          return NULL;
        for (int i = 0; i < len; i++) {
          Jsonv_Value item;
          if (jsonv_arr_get(arr, i, &item)) {
            res->u.array.items[i] = convert_jsonv_to_jsonata_impl(jsonata_arena, item, depth + 1);
            if (!res->u.array.items[i])
              return NULL;
          } else {
            res->u.array.items[i] = NULL;
          }
        }
      } else {
        res->u.array.items = NULL;
      }
      break;
    }
    case JSONV_VAL_OBJ: {
      res->type = JSONATA_VAL_OBJECT;
      Jsonv_Obj *obj = v.as.p;

      // If the object contains a streaming file path, delegate property lookups
      // dynamically to prevent loading the entire payload in RAM.
      Jsonv_Value stream_val;
      if (jsonv_obj_get(obj, "_stream_file_path", &stream_val) && stream_val.tag == JSONV_VAL_STRING) {
        res->get_property = nestor_lazy_get_property;
        NestorLazyContext *lazy_ctx = (NestorLazyContext *)jsonata_arena_alloc(jsonata_arena, sizeof(NestorLazyContext));
        if (lazy_ctx) {
          lazy_ctx->path = (const char *)stream_val.as.p;
          lazy_ctx->jsonata_arena = jsonata_arena;
          res->context = lazy_ctx;
        }
        break;
      }

      int len = jsonv_obj_length(obj);
      res->u.obj.count = len;
      if (len > 0) {
        res->u.obj.members = jsonata_arena_alloc(jsonata_arena, len * sizeof(Jsonata_Member));
        if (!res->u.obj.members)
          return NULL;
        for (int i = 0; i < len; i++) {
          const char *key = jsonv_obj_key_at(obj, i);
          Jsonv_Value val = jsonv_obj_val_at(obj, i);
          res->u.obj.members[i].key.ptr = key;
          res->u.obj.members[i].key.len = strlen(key);
          res->u.obj.members[i].value = convert_jsonv_to_jsonata_impl(jsonata_arena, val, depth + 1);
          if (!res->u.obj.members[i].value)
            return NULL;
        }
      } else {
        res->u.obj.members = NULL;
      }
      break;
    }
    default:
      res->type = JSONATA_VAL_UNDEFINED;
      break;
  }
  return res;
  /*#endregion*/
}

static Jsonata_Value *convert_jsonv_to_jsonata(Jsonata_Arena *jsonata_arena, Jsonv_Value v) {
  return convert_jsonv_to_jsonata_impl(jsonata_arena, v, 0);
}

static char *allocate_jsonv_string(Arena *arena, const char *data, size_t len) {
  /*#region*/
  size_t total_size = sizeof(uint32_t) + len + 1;
  char *buf = na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)len;
  char *str_ptr = buf + sizeof(uint32_t);
  memcpy(str_ptr, data, len);
  str_ptr[len] = '\0';
  return str_ptr;
  /*#endregion*/
}

// Converts a Jsonata_Value (returned by the jsonata engine) recursively back
// into a Jsonv_Value, allocating objects and strings in our custom chained arena.
static Jsonv_Value convert_jsonata_to_jsonv(Arena *our_arena, Jsonv_Arena *jsonv_arena, const Jsonata_Value *v) {
  /*#region*/
  if (!v)
    return jsonv_val_undefined();

  switch (v->type) {
    case JSONATA_VAL_UNDEFINED:
      return jsonv_val_undefined();
    case JSONATA_VAL_NULL:
      return jsonv_val_null();
    case JSONATA_VAL_BOOL:
      return jsonv_val_bool(v->u.b);
    case JSONATA_VAL_NUMBER:
      if (v->u.n == (double)(int64_t)v->u.n) {
        return jsonv_val_int((int64_t)v->u.n);
      } else {
        return jsonv_val_double(v->u.n);
      }
    case JSONATA_VAL_STRING: {
      char *str = allocate_jsonv_string(our_arena, v->u.s.ptr, v->u.s.len);
      if (!str)
        return jsonv_val_undefined();
      return jsonv_val_str(str);
    }
    case JSONATA_VAL_ARRAY: {
      Jsonv_Arr *arr = jsonv_arr_new(jsonv_arena);
      if (!arr)
        return jsonv_val_undefined();
      for (size_t i = 0; i < v->u.array.count; i++) {
        Jsonv_Value item = convert_jsonata_to_jsonv(our_arena, jsonv_arena, v->u.array.items[i]);
        jsonv_arr_set(jsonv_arena, arr, (int)i, item);
      }
      return jsonv_val_arr(arr);
    }
    case JSONATA_VAL_OBJECT: {
      Jsonv_Obj *obj = jsonv_obj_new(jsonv_arena, NULL);
      if (!obj)
        return jsonv_val_undefined();
      for (size_t i = 0; i < v->u.obj.count; i++) {
        char *key = allocate_jsonv_string(our_arena, v->u.obj.members[i].key.ptr, v->u.obj.members[i].key.len);
        if (!key)
          return jsonv_val_undefined();

        Jsonv_Value val = convert_jsonata_to_jsonv(our_arena, jsonv_arena, v->u.obj.members[i].value);
        jsonv_obj_set(jsonv_arena, obj, key, val);
      }
      return jsonv_val_obj(obj);
    }
    default:
      return jsonv_val_undefined();
  }
  /*#endregion*/
}

Jsonata_Arena *nestor_jsonata_arena_new(Arena *arena) {
  /*#region*/
  return jsonata_arena_new_custom(&my_jsonata_ops, arena);
  /*#endregion*/
}

int32_t evaluate_expression(Arena *arena, StringView expr, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value *out_val) {
  /*#region*/
  if (!arena || !jsonv_arena || !out_val)
    return ERR_OOM;

  // Create JSONata arena via our custom allocator operations mapping directly into our Arena
  Jsonata_Arena *jsonata_arena = jsonata_arena_new_custom(&my_jsonata_ops, arena);
  if (!jsonata_arena)
    return ERR_OOM;

  // Trim leading/trailing whitespace first to handle block scalar trailing newlines
  StringView raw_expr = expr;
  while (raw_expr.length > 0 && (raw_expr.data[0] == ' ' || raw_expr.data[0] == '\t' || raw_expr.data[0] == '\n' || raw_expr.data[0] == '\r')) {
    raw_expr.data++;
    raw_expr.length--;
  }
  while (raw_expr.length > 0 && (raw_expr.data[raw_expr.length - 1] == ' ' || raw_expr.data[raw_expr.length - 1] == '\t' || raw_expr.data[raw_expr.length - 1] == '\n' || raw_expr.data[raw_expr.length - 1] == '\r')) {
    raw_expr.length--;
  }

  // Strip ${{ and }} boundaries if present
  if (raw_expr.length >= 5 && raw_expr.data[0] == '$' && raw_expr.data[1] == '{' && raw_expr.data[2] == '{' &&
      raw_expr.data[raw_expr.length - 2] == '}' && raw_expr.data[raw_expr.length - 1] == '}') {
    raw_expr.data += 3;
    raw_expr.length -= 5;
  }

  // Trim whitespace inside boundaries
  while (raw_expr.length > 0 && (raw_expr.data[0] == ' ' || raw_expr.data[0] == '\t' || raw_expr.data[0] == '\n' || raw_expr.data[0] == '\r')) {
    raw_expr.data++;
    raw_expr.length--;
  }
  while (raw_expr.length > 0 && (raw_expr.data[raw_expr.length - 1] == ' ' || raw_expr.data[raw_expr.length - 1] == '\t' || raw_expr.data[raw_expr.length - 1] == '\n' || raw_expr.data[raw_expr.length - 1] == '\r')) {
    raw_expr.length--;
  }

  // Convert context value to Jsonata_Value
  Jsonata_Value *input_val = convert_jsonv_to_jsonata(jsonata_arena, context_val);
  if (!input_val) {
    return ERR_OOM;
  }

  // Null-terminate the expression string from the Arena before compiling.
  char *expr_cstr = na_alloc(arena, raw_expr.length + 1);
  if (!expr_cstr) {
    return ERR_OOM;
  }
  memcpy(expr_cstr, raw_expr.data, raw_expr.length);
  expr_cstr[raw_expr.length] = '\0';

  // Compile JSONata expression
  Jsonata_Error err = {0};
  Jsonata_Compiled *compiled = jsonata_compile(expr_cstr, raw_expr.length, jsonata_arena, &err);
  if (!compiled) {
    return ERR_MISSING_VAR;
  }

  // Evaluate Compiled Expression
  Jsonata_Value *res_val = jsonata_evaluate(compiled, input_val, jsonata_arena, &err);
  if (!res_val || res_val->type == JSONATA_VAL_UNDEFINED) {
    return ERR_MISSING_VAR;
  }

  // Convert result back to Jsonv_Value
  *out_val = convert_jsonata_to_jsonv(arena, jsonv_arena, res_val);
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t resolve_string(Arena *arena, StringView sv, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StringView *out_sv) {
  /*#region*/
  size_t buf_cap = sv.length + 256;
  char *buf = na_alloc(arena, buf_cap);
  if (!buf) return ERR_OOM;
  size_t buf_len = 0;

  size_t i = 0;
  while (i < sv.length) {
    if (i + 3 <= sv.length && sv.data[i] == '$' && sv.data[i+1] == '{' && sv.data[i+2] == '{') {
      size_t j = i + 3;
      while (j + 1 < sv.length && !(sv.data[j] == '}' && sv.data[j+1] == '}')) {
        j++;
      }
      if (j + 1 < sv.length) {
        StringView expr_sv = { sv.data + i + 3, j - (i + 3) };
        Jsonv_Value eval_res = jsonv_val_undefined();
        int32_t status = evaluate_expression(arena, expr_sv, jsonv_arena, context_val, &eval_res);
        if (status != ERR_SUCCESS) {
          return status;
        }
        if (eval_res.tag == JSONV_VAL_STRING) {
          size_t len = jsonv_val_str_len(eval_res);
          const char *str = (const char *)eval_res.as.p;
          if (buf_len + len >= buf_cap) {
            buf_cap = buf_len + len + 256;
            char *new_buf = na_alloc(arena, buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, buf, buf_len);
            buf = new_buf;
          }
          memcpy(buf + buf_len, str, len);
          buf_len += len;
        } else if (eval_res.tag == JSONV_VAL_INT) {
          char tmp[32];
          int len = snprintf(tmp, sizeof(tmp), "%lld", (long long)eval_res.as.i);
          if (buf_len + len >= buf_cap) {
            buf_cap = buf_len + len + 256;
            char *new_buf = na_alloc(arena, buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, buf, buf_len);
            buf = new_buf;
          }
          memcpy(buf + buf_len, tmp, len);
          buf_len += len;
        } else if (eval_res.tag == JSONV_VAL_DOUBLE) {
          char tmp[32];
          int len = snprintf(tmp, sizeof(tmp), "%g", eval_res.as.d);
          if (buf_len + len >= buf_cap) {
            buf_cap = buf_len + len + 256;
            char *new_buf = na_alloc(arena, buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, buf, buf_len);
            buf = new_buf;
          }
          memcpy(buf + buf_len, tmp, len);
          buf_len += len;
        } else if (eval_res.tag == JSONV_VAL_BOOLEAN) {
          const char *val_str = eval_res.as.boolean ? "true" : "false";
          size_t len = strlen(val_str);
          if (buf_len + len >= buf_cap) {
            buf_cap = buf_len + len + 256;
            char *new_buf = na_alloc(arena, buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, buf, buf_len);
            buf = new_buf;
          }
          memcpy(buf + buf_len, val_str, len);
          buf_len += len;
        }
        i = j + 2;
      } else {
        if (buf_len + (sv.length - i) >= buf_cap) {
          buf_cap = buf_len + (sv.length - i) + 256;
          char *new_buf = na_alloc(arena, buf_cap);
          if (!new_buf) return ERR_OOM;
          memcpy(new_buf, buf, buf_len);
          buf = new_buf;
        }
        memcpy(buf + buf_len, sv.data + i, sv.length - i);
        buf_len += (sv.length - i);
        break;
      }
    } else {
      if (buf_len + 1 >= buf_cap) {
        buf_cap = buf_len + 256;
        char *new_buf = na_alloc(arena, buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, buf, buf_len);
        buf = new_buf;
      }
      buf[buf_len++] = sv.data[i++];
    }
  }
  buf[buf_len] = '\0';
  out_sv->data = buf;
  out_sv->length = buf_len;
  return ERR_SUCCESS;
  /*#endregion*/
}

Jsonv_Value resolve_json_value(Arena *arena, Jsonv_Value v, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val) {
  /*#region*/
  switch (v.tag) {
    case JSONV_VAL_STRING: {
      StringView orig_sv = { (const char *)v.as.p, jsonv_val_str_len(v) };

      StringView trimmed = orig_sv;
      while (trimmed.length > 0 && (trimmed.data[0] == ' ' || trimmed.data[0] == '\t' || trimmed.data[0] == '\n' || trimmed.data[0] == '\r')) {
        trimmed.data++;
        trimmed.length--;
      }
      while (trimmed.length > 0 && (trimmed.data[trimmed.length - 1] == ' ' || trimmed.data[trimmed.length - 1] == '\t' || trimmed.data[trimmed.length - 1] == '\n' || trimmed.data[trimmed.length - 1] == '\r')) {
        trimmed.length--;
      }

      if (trimmed.length >= 5 && trimmed.data[0] == '$' && trimmed.data[1] == '{' && trimmed.data[2] == '{' &&
          trimmed.data[trimmed.length - 2] == '}' && trimmed.data[trimmed.length - 1] == '}') {
        size_t close_pos = 3;
        while (close_pos + 1 < trimmed.length - 2 && !(trimmed.data[close_pos] == '}' && trimmed.data[close_pos + 1] == '}')) {
          close_pos++;
        }
        if (close_pos + 1 == trimmed.length - 2) {
          StringView expr_sv = { trimmed.data + 3, trimmed.length - 5 };
          Jsonv_Value eval_res = jsonv_val_undefined();
          if (evaluate_expression(arena, expr_sv, jsonv_arena, context_val, &eval_res) == ERR_SUCCESS) {
            return eval_res;
          }
        }
      }

      StringView resolved_sv;
      if (resolve_string(arena, orig_sv, jsonv_arena, context_val, &resolved_sv) == ERR_SUCCESS) {
        char *str = allocate_jsonv_string(arena, resolved_sv.data, resolved_sv.length);
        if (str) return jsonv_val_str(str);
      }
      return v;
    }
    case JSONV_VAL_ARRAY: {
      Jsonv_Arr *arr = v.as.p;
      int len = jsonv_arr_length(arr);
      Jsonv_Arr *new_arr = jsonv_arr_new(jsonv_arena);
      for (int k = 0; k < len; k++) {
        Jsonv_Value item;
        if (jsonv_arr_get(arr, k, &item)) {
          jsonv_arr_set(jsonv_arena, new_arr, k, resolve_json_value(arena, item, jsonv_arena, context_val));
        }
      }
      return jsonv_val_arr(new_arr);
    }
    case JSONV_VAL_OBJ: {
      Jsonv_Obj *obj = v.as.p;
      int len = jsonv_obj_length(obj);
      Jsonv_Obj *new_obj = jsonv_obj_new(jsonv_arena, NULL);
      for (int k = 0; k < len; k++) {
        const char *key = jsonv_obj_key_at(obj, k);
        Jsonv_Value val = jsonv_obj_val_at(obj, k);
        char *new_key = allocate_jsonv_string(arena, key, strlen(key));
        jsonv_obj_set(jsonv_arena, new_obj, new_key, resolve_json_value(arena, val, jsonv_arena, context_val));
      }
      return jsonv_val_obj(new_obj);
    }
    default:
      return v;
  }
  /*#endregion*/
}

static int32_t serialize_value_to_buf(Arena *arena, Jsonv_Value v, char **buf_ptr, size_t *buf_len, size_t *buf_cap) {
  /*#region*/
  switch (v.tag) {
    case JSONV_VAL_UNDEFINED:
    case JSONV_VAL_NULL: {
      const char *s = "null";
      size_t len = 4;
      if (*buf_len + len >= *buf_cap) {
        *buf_cap = *buf_len + len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      memcpy(*buf_ptr + *buf_len, s, len);
      *buf_len += len;
      break;
    }
    case JSONV_VAL_BOOLEAN: {
      const char *s = v.as.boolean ? "true" : "false";
      size_t len = strlen(s);
      if (*buf_len + len >= *buf_cap) {
        *buf_cap = *buf_len + len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      memcpy(*buf_ptr + *buf_len, s, len);
      *buf_len += len;
      break;
    }
    case JSONV_VAL_INT: {
      char tmp[64];
      int len = snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.i);
      if (*buf_len + len >= *buf_cap) {
        *buf_cap = *buf_len + len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      memcpy(*buf_ptr + *buf_len, tmp, len);
      *buf_len += len;
      break;
    }
    case JSONV_VAL_DOUBLE: {
      char tmp[64];
      int len = snprintf(tmp, sizeof(tmp), "%g", v.as.d);
      if (*buf_len + len >= *buf_cap) {
        *buf_cap = *buf_len + len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      memcpy(*buf_ptr + *buf_len, tmp, len);
      *buf_len += len;
      break;
    }
    case JSONV_VAL_STRING: {
      size_t str_len = jsonv_val_str_len(v);
      const char *str = (const char *)v.as.p;
      size_t required = 2 * str_len + 3;
      if (*buf_len + required >= *buf_cap) {
        *buf_cap = *buf_len + required + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      char *d = *buf_ptr + *buf_len;
      *d++ = '"';
      for (size_t k = 0; k < str_len; k++) {
        char c = str[k];
        if (c == '"') { *d++ = '\\'; *d++ = '"'; }
        else if (c == '\\') { *d++ = '\\'; *d++ = '\\'; }
        else if (c == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else if (c == '\r') { *d++ = '\\'; *d++ = 'r'; }
        else if (c == '\t') { *d++ = '\\'; *d++ = 't'; }
        else { *d++ = c; }
      }
      *d++ = '"';
      *buf_len = d - *buf_ptr;
      break;
    }
    case JSONV_VAL_ARRAY: {
      Jsonv_Arr *arr = v.as.p;
      int len = jsonv_arr_length(arr);
      if (*buf_len + 1 >= *buf_cap) {
        *buf_cap = *buf_len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      (*buf_ptr)[(*buf_len)++] = '[';
      for (int k = 0; k < len; k++) {
        if (k > 0) {
          if (*buf_len + 2 >= *buf_cap) {
            *buf_cap = *buf_len + 256;
            char *new_buf = na_alloc(arena, *buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, *buf_ptr, *buf_len);
            *buf_ptr = new_buf;
          }
          (*buf_ptr)[(*buf_len)++] = ',';
          (*buf_ptr)[(*buf_len)++] = ' ';
        }
        Jsonv_Value item;
        if (jsonv_arr_get(arr, k, &item)) {
          int32_t status = serialize_value_to_buf(arena, item, buf_ptr, buf_len, buf_cap);
          if (status != ERR_SUCCESS) return status;
        }
      }
      if (*buf_len + 1 >= *buf_cap) {
        *buf_cap = *buf_len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      (*buf_ptr)[(*buf_len)++] = ']';
      break;
    }
    case JSONV_VAL_OBJ: {
      Jsonv_Obj *obj = v.as.p;
      int len = jsonv_obj_length(obj);
      if (*buf_len + 1 >= *buf_cap) {
        *buf_cap = *buf_len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      (*buf_ptr)[(*buf_len)++] = '{';
      for (int k = 0; k < len; k++) {
        if (k > 0) {
          if (*buf_len + 2 >= *buf_cap) {
            *buf_cap = *buf_len + 256;
            char *new_buf = na_alloc(arena, *buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, *buf_ptr, *buf_len);
            *buf_ptr = new_buf;
          }
          (*buf_ptr)[(*buf_len)++] = ',';
          (*buf_ptr)[(*buf_len)++] = ' ';
        }
        const char *key = jsonv_obj_key_at(obj, k);
        Jsonv_Value val = jsonv_obj_val_at(obj, k);
        size_t key_len = strlen(key);
        size_t required = key_len + 4;
        if (*buf_len + required >= *buf_cap) {
          *buf_cap = *buf_len + required + 256;
          char *new_buf = na_alloc(arena, *buf_cap);
          if (!new_buf) return ERR_OOM;
          memcpy(new_buf, *buf_ptr, *buf_len);
          *buf_ptr = new_buf;
        }
        char *d = *buf_ptr + *buf_len;
        *d++ = '"';
        memcpy(d, key, key_len);
        d += key_len;
        *d++ = '"';
        *d++ = ':';
        *d++ = ' ';
        *buf_len = d - *buf_ptr;
        int32_t status = serialize_value_to_buf(arena, val, buf_ptr, buf_len, buf_cap);
        if (status != ERR_SUCCESS) return status;
      }
      if (*buf_len + 1 >= *buf_cap) {
        *buf_cap = *buf_len + 256;
        char *new_buf = na_alloc(arena, *buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, *buf_ptr, *buf_len);
        *buf_ptr = new_buf;
      }
      (*buf_ptr)[(*buf_len)++] = '}';
      break;
    }
    default:
      break;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t serialize_jsonv_value(Arena *arena, Jsonv_Value v, char **out_str) {
  /*#region*/
  size_t cap = 256;
  char *buf = na_alloc(arena, cap);
  if (!buf) return ERR_OOM;
  size_t len = 0;
  int32_t status = serialize_value_to_buf(arena, v, &buf, &len, &cap);
  if (status != ERR_SUCCESS) return status;
  
  buf[len] = '\0';
  *out_str = buf;
  return ERR_SUCCESS;
  /*#endregion*/
}

bool is_truthy(Jsonv_Value v) {
  /*#region*/
  if (v.tag == JSONV_VAL_BOOLEAN) return v.as.boolean;
  if (v.tag == JSONV_VAL_INT) return v.as.i != 0;
  if (v.tag == JSONV_VAL_DOUBLE) return v.as.d != 0.0;
  if (v.tag == JSONV_VAL_STRING) return jsonv_val_str_len(v) > 0;
  if (v.tag == JSONV_VAL_NULL || v.tag == JSONV_VAL_UNDEFINED) return false;
  return true;
  /*#endregion*/
}

long parse_duration_ms(StringView sv) {
  /*#region*/
  long val = 0;
  size_t idx = 0;
  while (idx < sv.length && sv.data[idx] >= '0' && sv.data[idx] <= '9') {
    val = val * 10 + (sv.data[idx] - '0');
    idx++;
  }
  StringView unit = { sv.data + idx, sv.length - idx };
  if (sv_equals_cstr(unit, "ms")) {
    return val;
  } else if (sv_equals_cstr(unit, "s") || unit.length == 0) {
    return val * 1000;
  }
  return 1000;
  /*#endregion*/
}
