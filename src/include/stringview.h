#ifndef _NESTOR_STRING_VIEW_H
#define _NESTOR_STRING_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include "arena.h"

typedef struct {
  const char *data;
  size_t length;
} StringView;

bool sv_equals_cstr(StringView sv, const char *cstr);
int sv_compare(StringView sv1, StringView sv2);
bool sv_starts_with(StringView sv, StringView prefix);
ptrdiff_t sv_find_char(StringView sv, char c);
char *sv_to_cstring(Arena *arena, StringView sv);

#endif

