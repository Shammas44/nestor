#include "utils.h"
#include "aho_corasick.h"
#include <string.h>

char *timestamp_to_string(size_t timestamp) {
  /*#region*/
  struct tm *local_time;
  static char str[20]; // "YYYY-MM-DD HH:MM:SS" + '\0'
  local_time = localtime((const time_t *)&timestamp);
  // Format it as: "YYYY-MM-DD HH:MM:SS"
  strftime(str, sizeof(str), "%H:%M:%S", local_time);
  return str;
  /*#endregion*/
}

void event_log(Keys key, const char *format, ...) {
  /*#region*/
  time_t t;
  time(&t);
  static char message_buffer[100 + 128];
  va_list args;
  va_start(args, format);
  vsnprintf(message_buffer, sizeof(message_buffer), format, args);
  va_end(args);
  char *start = KEY(key);
  char *end = KEY(Reset);
  char *date = timestamp_to_string(t);

  ACNode *ac_root = get_global_ac_root();
  if (ac_root) {
    static char redacted_buffer[512];
    redact_stream(ac_root, message_buffer, redacted_buffer, strlen(message_buffer));
    printf("[%s%s%s] %s%s%s\n", KEY(Magenta), date, KEY(Reset), start,
           redacted_buffer, end);
  } else {
    printf("[%s%s%s] %s%s%s\n", KEY(Magenta), date, KEY(Reset), start,
           message_buffer, end);
  }
  /*#endregion*/
}
