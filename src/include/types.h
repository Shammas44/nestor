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

typedef struct {
  bool header;
  char delimiter;
  bool relaxed;
} CsvOptions;

typedef enum {
  XML_PARKER,
  XML_BADGERFISH,
  XML_JSONML
} XmlConvention;

typedef enum {
  BINARY_BASE64,
  BINARY_HEX
} BinaryEncoding;

int32_t csv_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, CsvOptions opts, Jsonv_Value *out_val);
int32_t xml_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, XmlConvention conv, Jsonv_Value *out_val);
int32_t form_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, Jsonv_Value *out_val);
int32_t binary_decode(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, BinaryEncoding enc, Jsonv_Value *out_val);
int32_t yaml_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, Jsonv_Value *out_val);

int32_t json_to_csv(Arena *arena, Jsonv_Value val, CsvOptions opts, StringView *out_sv);
int32_t json_to_xml(Arena *arena, Jsonv_Value val, XmlConvention conv, StringView *out_sv);
int32_t json_to_form(Arena *arena, Jsonv_Value val, StringView *out_sv);
int32_t binary_encode(Arena *arena, StringView data, BinaryEncoding enc, StringView *out_sv);

#endif // _NESTOR_TYPES_H
