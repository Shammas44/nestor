#include "types.h"
#include <stdio.h>
#include <string.h>

static char *allocate_jsonv_string_from_sv(Arena *arena, StringView sv) {
  /*#region*/
  size_t total_size = sizeof(uint32_t) + sv.length + 1;
  char *buf = (char *)na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)sv.length;
  char *str_ptr = buf + sizeof(uint32_t);
  if (sv.length > 0 && sv.data) {
    memcpy(str_ptr, sv.data, sv.length);
  }
  str_ptr[sv.length] = '\0';
  return str_ptr;
  /*#endregion*/
}

int32_t transcode_xml_to_document(Arena *arena, Jsonv_Arena *jsonv_arena, const char *xml_data, size_t xml_len, Jsonv_Value *out_doc) {
  /*#region*/
  if (!xml_data || !out_doc) return ERR_INVALID_BOUNDARY;

  Jsonv_Obj *doc_obj = jsonv_obj_new(jsonv_arena, NULL);
  if (!doc_obj) return ERR_OOM;

  size_t pos = 0;
  while (pos < xml_len) {
    while (pos < xml_len && xml_data[pos] != '<') pos++;
    if (pos >= xml_len) break;

    pos++; // skip '<'
    if (pos < xml_len && (xml_data[pos] == '/' || xml_data[pos] == '?' || xml_data[pos] == '!')) {
      while (pos < xml_len && xml_data[pos] != '>') pos++;
      if (pos < xml_len) pos++;
      continue;
    }

    size_t tag_start = pos;
    while (pos < xml_len && xml_data[pos] != '>' && xml_data[pos] != ' ' && xml_data[pos] != '/') pos++;
    StringView tag_name = { xml_data + tag_start, pos - tag_start };

    while (pos < xml_len && xml_data[pos] != '>') pos++;
    if (pos < xml_len) pos++; // skip '>'

    size_t val_start = pos;
    while (pos < xml_len && xml_data[pos] != '<') pos++;
    StringView val_sv = { xml_data + val_start, pos - val_start };

    if (tag_name.length > 0 && val_sv.length > 0) {
      // Check non-whitespace
      bool has_text = false;
      for (size_t k = 0; k < val_sv.length; k++) {
        if (val_sv.data[k] != ' ' && val_sv.data[k] != '\t' && val_sv.data[k] != '\r' && val_sv.data[k] != '\n') {
          has_text = true;
          break;
        }
      }
      if (has_text) {
        char *key_str = allocate_jsonv_string_from_sv(arena, tag_name);
        char *val_str = allocate_jsonv_string_from_sv(arena, val_sv);
        if (key_str && val_str) {
          jsonv_obj_set(jsonv_arena, doc_obj, key_str, jsonv_val_str(val_str));
        }
      }
    }
  }

  *out_doc = jsonv_val_obj(doc_obj);
  return ERR_SUCCESS;

  /*#endregion*/
}

int32_t transcode_csv_to_table(Arena *arena, Jsonv_Arena *jsonv_arena, const char *csv_data, size_t csv_len, char delimiter, Jsonv_Value *out_table) {
  /*#region*/
  if (!csv_data || !out_table) return ERR_INVALID_BOUNDARY;
  if (delimiter == '\0') delimiter = ',';

  Jsonv_Arr *rows_arr = jsonv_arr_new(jsonv_arena);
  if (!rows_arr) return ERR_OOM;

  // Parse header line
  size_t pos = 0;
  StringView headers[64];
  size_t header_count = 0;

  while (pos < csv_len && csv_data[pos] != '\r' && csv_data[pos] != '\n') {
    size_t start = pos;
    while (pos < csv_len && csv_data[pos] != delimiter && csv_data[pos] != '\r' && csv_data[pos] != '\n') pos++;
    if (header_count < 64) {
      headers[header_count++] = (StringView){ csv_data + start, pos - start };
    }
    if (pos < csv_len && csv_data[pos] == delimiter) pos++;
  }
  while (pos < csv_len && (csv_data[pos] == '\r' || csv_data[pos] == '\n')) pos++;

  // Parse data rows
  int row_idx = 0;
  while (pos < csv_len) {
    if (csv_data[pos] == '\r' || csv_data[pos] == '\n') {
      pos++;
      continue;
    }

    Jsonv_Obj *row_obj = jsonv_obj_new(jsonv_arena, NULL);
    size_t col_idx = 0;

    while (pos < csv_len && csv_data[pos] != '\r' && csv_data[pos] != '\n') {
      size_t start = pos;
      while (pos < csv_len && csv_data[pos] != delimiter && csv_data[pos] != '\r' && csv_data[pos] != '\n') pos++;
      StringView cell_sv = { csv_data + start, pos - start };

      if (col_idx < header_count) {
        char *key_str = allocate_jsonv_string_from_sv(arena, headers[col_idx]);
        char *val_str = allocate_jsonv_string_from_sv(arena, cell_sv);
        if (key_str && val_str) {
          jsonv_obj_set(jsonv_arena, row_obj, key_str, jsonv_val_str(val_str));
        }
      }
      col_idx++;
      if (pos < csv_len && csv_data[pos] == delimiter) pos++;
    }
    while (pos < csv_len && (csv_data[pos] == '\r' || csv_data[pos] == '\n')) pos++;

    jsonv_arr_set(jsonv_arena, rows_arr, row_idx++, jsonv_val_obj(row_obj));
  }


  *out_table = jsonv_val_arr(rows_arr);
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t transcode_binary_to_value(Arena *arena, Jsonv_Arena *jsonv_arena, const uint8_t *data, size_t len, Jsonv_Value *out_val) {
  /*#region*/
  (void)jsonv_arena;
  if (!data || !out_val) return ERR_INVALID_BOUNDARY;

  StringView sv = { (const char *)data, len };
  char *str = allocate_jsonv_string_from_sv(arena, sv);
  if (!str) return ERR_OOM;

  *out_val = jsonv_val_str(str);
  return ERR_SUCCESS;
  /*#endregion*/
}
