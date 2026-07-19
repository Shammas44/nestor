#ifndef _JSONV_COMPAT_H
#define _JSONV_COMPAT_H

#define JSONV_YAML_SUPPORT
#include <jsonv/jsonv.h>

Jsonv_Obj *create_empty_jsonv_object(Jsonv_Arena *jsonv_arena);

#define jsonv_obj_new(arena, shape) ((shape) == NULL ? create_empty_jsonv_object(arena) : (jsonv_obj_new)(arena, shape))

#endif
