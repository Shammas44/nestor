#include "stringview.h"
#include <string.h>

bool sv_equals_cstr(StringView sv, const char *cstr) {
  size_t len = strlen(cstr);
  if (sv.length != len)
    return false;
  return memcmp(sv.data, cstr, len) == 0;
}
