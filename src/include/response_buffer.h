#ifndef _NESTOR_RESPONSE_BUFFER_H
#define _NESTOR_RESPONSE_BUFFER_H

#include "arena.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct ResponseHeaderNode ResponseHeaderNode;
struct ResponseHeaderNode {
  char *name;
  char *value;
  ResponseHeaderNode *next;
};

typedef struct {
  Arena *arena;
  char *buf;
  size_t len;
  size_t cap;
  char cache_control[256];
  char expires[128];
  char etag[128];
  char last_modified[128];
  ResponseHeaderNode *headers_head;
  bool is_stream;
  char stream_file_path[256];
  int stream_fd;
} ResponseBuffer;

#endif
