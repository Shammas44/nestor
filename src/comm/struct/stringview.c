#include "stringview.h"
#include <string.h>

bool sv_equals_cstr(StringView sv, const char *cstr) {
  /*#region*/
  size_t len = strlen(cstr);
  if (sv.length != len)
    return false;
  return memcmp(sv.data, cstr, len) == 0;
  /*#endregion*/
}

int sv_compare(StringView sv1, StringView sv2) {
  /*#region*/
  size_t min_len = sv1.length < sv2.length ? sv1.length : sv2.length;
  if (min_len > 0) {
    int cmp = memcmp(sv1.data, sv2.data, min_len);
    if (cmp != 0)
      return cmp;
  }
  if (sv1.length < sv2.length)
    return -1;
  if (sv1.length > sv2.length)
    return 1;
  return 0;
  /*#endregion*/
}

bool sv_starts_with(StringView sv, StringView prefix) {
  /*#region*/
  if (sv.length < prefix.length)
    return false;
  if (prefix.length == 0)
    return true;
  return memcmp(sv.data, prefix.data, prefix.length) == 0;
  /*#endregion*/
}

ptrdiff_t sv_find_char(StringView sv, char c) {
  /*#region*/
  for (size_t i = 0; i < sv.length; i++) {
    if (sv.data[i] == c) {
      return (ptrdiff_t)i;
    }
  }
  return -1;
  /*#endregion*/
}

char *sv_to_cstring(Arena *arena, StringView sv) {
  /*#region*/
  if (!arena)
    return NULL;
  char *str = na_alloc(arena, sv.length + 1);
  if (!str)
    return NULL;
  if (sv.length > 0 && sv.data != NULL) {
    memcpy(str, sv.data, sv.length);
  }
  str[sv.length] = '\0';
  return str;
  /*#endregion*/
}
