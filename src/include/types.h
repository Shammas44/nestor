#ifndef _NESTOR_TYPES_H
#define _NESTOR_TYPES_H

#include "arena.h"
#include "stringview.h"
#include "error_codes.h"
#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
  TYPE_SCALAR_STRING,
  TYPE_SCALAR_NUMBER,
  TYPE_SCALAR_BOOLEAN,
  TYPE_OBJECT,
  TYPE_LIST,
  TYPE_TABLE,
  TYPE_DOCUMENT,
  TYPE_BINARY,
  TYPE_STREAM
} CanonicalType;

int32_t transcode_xml_to_document(Arena *arena, Jsonv_Arena *jsonv_arena, const char *xml_data, size_t xml_len, Jsonv_Value *out_doc);
int32_t transcode_csv_to_table(Arena *arena, Jsonv_Arena *jsonv_arena, const char *csv_data, size_t csv_len, char delimiter, Jsonv_Value *out_table);
int32_t transcode_binary_to_value(Arena *arena, Jsonv_Arena *jsonv_arena, const uint8_t *data, size_t len, Jsonv_Value *out_val);

#endif // _NESTOR_TYPES_H
