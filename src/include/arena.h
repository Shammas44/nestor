#ifndef _NESTOR_ARENA_H
#define _NESTOR_ARENA_H
#include <stddef.h>

typedef struct {
  char *buffer;
  size_t capacity;
  size_t offset;
} Arena;

Arena *arena_create(size_t capacity);
void *arena_alloc(Arena *arena, size_t size);
void arena_destroy(Arena *arena);
size_t arena_checkpoint(Arena *arena);
void arena_restore(Arena *arena, size_t checkpoint);

#endif
