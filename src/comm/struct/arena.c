#include "arena.h"
#include <stdlib.h>
#include <stdint.h>

// Alignment helper: rounds up size to the nearest multiple of align. Since align is a power of 2,
// (align - 1) creates a bitmask of the lower bits, and ~ (align - 1) clears them.
#define ALIGN_UP(size, align) (((size) + ((align) - 1)) & ~((align) - 1))

void arena_init(Arena *arena, size_t default_chunk_size) {
  /*#region*/
  if (!arena)
    return;
  arena->first = NULL;
  arena->current = NULL;
  arena->default_chunk_size = default_chunk_size;
  /*#endregion*/
}

Arena *arena_create(size_t capacity) {
  /*#region*/
  Arena *arena = malloc(sizeof(Arena));
  if (!arena)
    return NULL;
  arena_init(arena, capacity);
  return arena;
  /*#endregion*/
}

void *na_alloc(Arena *arena, size_t size) {
  /*#region*/
  if (!arena)
    return NULL;

  // We align the size to 16 bytes. Since the starting address of each chunk's
  // buffer is aligned by malloc (which is at least 16-byte aligned on 64-bit macOS),
  // keeping the offset 16-byte aligned guarantees all returned pointers are 16-byte aligned.
  size_t aligned_size = ALIGN_UP(size, 16);

  // Try to fit the allocation in the current chunk if one is active.
  if (arena->current) {
    if (arena->current->offset + aligned_size <= arena->current->capacity) {
      void *ptr = arena->current->memory + arena->current->offset;
      arena->current->offset += aligned_size;
      return ptr;
    }
  }

  // Current chunk is either NULL or full. Check if a pre-allocated next chunk
  // exists in the chain and has enough capacity.
  ArenaChunk *next_chunk = arena->current ? arena->current->next : NULL;
  if (next_chunk) {
    next_chunk->offset = 0;
    if (aligned_size <= next_chunk->capacity) {
      arena->current = next_chunk;
      void *ptr = next_chunk->memory;
      next_chunk->offset = aligned_size;
      return ptr;
    }
  }

  // No suitable chunk exists. Allocate a new chunk from the OS.
  // The capacity must be at least the requested size or the default chunk size.
  size_t chunk_capacity = arena->default_chunk_size;
  if (aligned_size > chunk_capacity) {
    chunk_capacity = aligned_size;
  }

  ArenaChunk *chunk = malloc(sizeof(ArenaChunk));
  if (!chunk)
    return NULL;

  chunk->memory = malloc(chunk_capacity);
  if (!chunk->memory) {
    free(chunk);
    return NULL;
  }

  chunk->capacity = chunk_capacity;
  chunk->offset = aligned_size;
  chunk->next = NULL;

  // Link the new chunk into the chain.
  if (!arena->first) {
    arena->first = chunk;
    arena->current = chunk;
  } else {
    ArenaChunk *last = arena->first;
    while (last->next) {
      last = last->next;
    }
    last->next = chunk;
    arena->current = chunk;
  }

  return chunk->memory;
  /*#endregion*/
}

void arena_reset(Arena *arena) {
  /*#region*/
  if (!arena)
    return;
  // Resets offsets of all chunks in the chain to 0. This allows reusing the
  // already allocated OS memory without freeing it, avoiding malloc/free overhead.
  ArenaChunk *c = arena->first;
  while (c) {
    c->offset = 0;
    c = c->next;
  }
  arena->current = arena->first;
  /*#endregion*/
}

void arena_destroy(Arena *arena) {
  /*#region*/
  if (!arena)
    return;
  // Deep-free all chunks in the chain, returning memory to the system.
  ArenaChunk *c = arena->first;
  while (c) {
    ArenaChunk *next = c->next;
    free(c->memory);
    free(c);
    c = next;
  }
  free(arena);
  /*#endregion*/
}

size_t arena_checkpoint(Arena *arena) {
  /*#region*/
  if (!arena || !arena->first)
    return 0;

  // Find the index of the current chunk in the chain to pack it with the offset.
  size_t index = 0;
  ArenaChunk *c = arena->first;
  while (c && c != arena->current) {
    c = c->next;
    index++;
  }

  size_t offset = arena->current ? arena->current->offset : 0;
  // Pack the chunk index (upper 32 bits) and the current chunk's offset (lower 32 bits)
  // into a single size_t. This is safe on 64-bit systems.
  return (index << 32) | (offset & 0xFFFFFFFFULL);
  /*#endregion*/
}

void arena_restore(Arena *arena, size_t checkpoint) {
  /*#region*/
  if (!arena || !arena->first)
    return;

  // Unpack chunk index and offset.
  size_t index = checkpoint >> 32;
  size_t offset = checkpoint & 0xFFFFFFFFULL;

  size_t i = 0;
  ArenaChunk *c = arena->first;
  while (c && i < index) {
    c = c->next;
    i++;
  }

  if (c) {
    arena->current = c;
    c->offset = offset;
    // All subsequent chunks must be reset to offset 0, since their allocations are discarded.
    c = c->next;
    while (c) {
      c->offset = 0;
      c = c->next;
    }
  }
  /*#endregion*/
}
