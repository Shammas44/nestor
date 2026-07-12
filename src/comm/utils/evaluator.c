#include "evaluator.h"
#include <jsonata/jsonata.h>
#include <string.h>
#include <stdio.h>

void *na_alloc(Arena *arena, size_t size);

static void *my_jsonata_arena_alloc(void *user_data, size_t size) {
  /*#region*/
  return na_alloc((Arena *)user_data, size);
  /*#endregion*/
}

static void my_jsonata_arena_reset(void *user_data) {
  /*#region*/
  arena_reset((Arena *)user_data);
  /*#endregion*/
}

static void my_jsonata_arena_reset_to(void *user_data, size_t keep_size) {
  /*#region*/
  arena_restore((Arena *)user_data, keep_size);
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
static Jsonata_Value *convert_jsonv_to_jsonata(Jsonata_Arena *jsonata_arena, Jsonv_Value v) {
  /*#region*/
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
            res->u.array.items[i] = convert_jsonv_to_jsonata(jsonata_arena, item);
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
          res->u.obj.members[i].value = convert_jsonv_to_jsonata(jsonata_arena, val);
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

int32_t evaluate_expression(Arena *arena, StringView expr, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value *out_val) {
  /*#region*/
  if (!arena || !jsonv_arena || !out_val)
    return ERR_OOM;

  // Create JSONata arena via our custom allocator operations mapping directly into our Arena
  Jsonata_Arena *jsonata_arena = jsonata_arena_new_custom(&my_jsonata_ops, arena);
  if (!jsonata_arena)
    return ERR_OOM;

  // Strip ${{ and }} boundaries if present
  StringView raw_expr = expr;
  if (expr.length >= 4 && expr.data[0] == '$' && expr.data[1] == '{' && expr.data[2] == '{' &&
      expr.data[expr.length - 2] == '}' && expr.data[expr.length - 1] == '}') {
    raw_expr.data = expr.data + 3;
    raw_expr.length = expr.length - 5;
  }

  // Trim whitespace
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

  // Compile JSONata expression
  Jsonata_Error err = {0};
  Jsonata_Compiled *compiled = jsonata_compile(raw_expr.data, raw_expr.length, jsonata_arena, &err);
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
