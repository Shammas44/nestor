#include "arena.h"
#include <stdlib.h>

Arena *arena_create(size_t capacity) {
/*#region*/
  Arena *arena = malloc(sizeof(Arena));
  if (!arena)
    return NULL;
  arena->buffer = malloc(capacity);
  arena->capacity = capacity;
  arena->offset = 0;
  return arena;
/*#endregion*/
}

void *arena_alloc(Arena *arena, size_t size) {
/*#region*/
  if (arena->offset + size > arena->capacity)
    return NULL;
  void *ptr = arena->buffer + arena->offset;
  arena->offset += size;
  return ptr;
/*#endregion*/
}

void arena_destroy(Arena *arena) {
/*#region*/
  if (arena) {
    free(arena->buffer);
    free(arena);
  }
/*#endregion*/
}

// Takes a snapshot of the current memory allocation boundary
size_t arena_checkpoint(Arena *arena) {
/*#region*/
  if (!arena)
    return 0;
  return arena->offset;
/*#endregion*/
}

// Rewinds the memory boundary to a previous snapshot,
// instantly discarding any allocations made after the checkpoint.
void arena_restore(Arena *arena, size_t checkpoint) {
/*#region*/
  if (arena && checkpoint <= arena->capacity) {
    arena->offset = checkpoint;
  }
/*#endregion*/
}
