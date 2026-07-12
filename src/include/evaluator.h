#ifndef _NESTOR_EVALUATOR_H
#define _NESTOR_EVALUATOR_H

#include "arena.h"
#include "ast.h"
#include "stringview.h"
#include "error_codes.h"
#include <jsonv/jsonv.h>

int32_t evaluate_expression(Arena *arena, StringView expr, Jsonv_Arena *jsonv_arena, Jsonv_Value context_val, Jsonv_Value *out_val);

#endif
