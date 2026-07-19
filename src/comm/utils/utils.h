#ifndef _NESTOR_UTILS_H
#define _NESTOR_UTILS_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define KB(x) 1024 * x
#define MB(x) 1024 * 1024 * x

static char *colors[] = {
    "\x1b[30m", "\x1b[31m", "\x1b[32m", "\x1b[33m", "\x1b[34m",
    "\x1b[35m", "\x1b[36m", "\x1b[37m", "\x1b[0m",
};

typedef enum {
  Black,
  Red,
  Green,
  Yellow,
  Blue,
  Magenta,
  Cyan,
  White,
  Reset,
} Keys;

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define MAX_PATH_LENGTH 1024
#define INITIAL_CAPACITY 16
#define KEY(index)                                                             \
  ({                                                                           \
    char *_key = colors[index];                                                \
    _key;                                                                      \
  })

char *timestamp_to_string(size_t timestamp);

void event_log(Keys key, const char *format, ...);

#endif
