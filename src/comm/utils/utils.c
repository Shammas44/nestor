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

#include <jsonv/jsonv.h>

Jsonv_Obj *create_empty_jsonv_object(Jsonv_Arena *jsonv_arena) {
  /*#region*/
  Jsonv_Config config = {0};
  config.default_block_size = 4096;
  config.max_limit = 1024 * 1024;
  config.max_depth = 10;
  config.max_values = 100;
  config.max_objects = 10;
  config.max_array = 10;
  config.max_string_bytes = 1024;

  Jsonv_Arena_Error err = 0;
  Jsonv_Context *temp_ctx = jsonv_ctx_new(jsonv_arena, &config, &err);
  if (!temp_ctx) return NULL;

  if (!jsonv_ctx_parse_data(temp_ctx, (const unsigned char *)"{}")) {
    return NULL;
  }

  Jsonv_Value val;
  if (!jsonv_ctx_get_value(temp_ctx, &val) || val.tag != JSONV_VAL_OBJ) {
    return NULL;
  }

  return val.as.p;
  /*#endregion*/
}
