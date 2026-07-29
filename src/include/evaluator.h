#ifndef _NESTOR_EVALUATOR_H
#define _NESTOR_EVALUATOR_H

#include "arena.h"
#include "ast.h"
#include "stringview.h"
#include "error_codes.h"
#include <jsonv/jsonv.h>
#include <stdbool.h>

int32_t evaluate_expression(Arena *arena, StringView expr, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value *out_val);
int32_t resolve_string(Arena *arena, StringView sv, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, StringView *out_sv);
Jsonv_Value resolve_json_value(Arena *arena, Jsonv_Value v, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val);
int32_t serialize_jsonv_value(Arena *arena, Jsonv_Value v, char **out_str);
bool is_truthy(Jsonv_Value v);
long parse_duration_ms(StringView sv);
struct Jsonata_Env; // opaque forward declaration
typedef struct Jsonata_Arena Jsonata_Arena;
Jsonata_Arena *nestor_jsonata_arena_new(Arena *arena);

#endif
