#ifndef _NESTOR_STRING_VIEW_H
#define _NESTOR_STRING_VIEW_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *data;
  size_t length;
} StringView;

bool sv_equals_cstr(StringView sv, const char *cstr);

#endif
