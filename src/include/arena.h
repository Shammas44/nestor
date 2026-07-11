#ifndef _NESTOR_ARENA_H
#define _NESTOR_ARENA_H
#include <stddef.h>
#include <stdint.h>

typedef struct ArenaChunk ArenaChunk;
struct ArenaChunk {
  uint8_t *memory;
  size_t capacity;
  size_t offset;
  ArenaChunk *next;
};

typedef struct {
  ArenaChunk *first;
  ArenaChunk *current;
  size_t default_chunk_size;
} Arena;

Arena *arena_create(size_t capacity);
void arena_init(Arena *arena, size_t default_chunk_size);
void *na_alloc(Arena *arena, size_t size);
void arena_reset(Arena *arena);
void arena_destroy(Arena *arena);
size_t arena_checkpoint(Arena *arena);
void arena_restore(Arena *arena, size_t checkpoint);

#endif

