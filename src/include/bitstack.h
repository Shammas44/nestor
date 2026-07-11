#ifndef _NESTOR_BITSTACK_H
#define _NESTOR_BITSTACK_H

#include <stddef.h>
#include <stdint.h>
#include "arena.h"

typedef struct {
  uint64_t *data;
  size_t top;
  size_t capacity;
} BitStack;

int32_t bitstack_init(BitStack *stack, Arena *arena, size_t capacity);
int32_t bitstack_push(BitStack *stack, uint64_t value);
int32_t bitstack_pop(BitStack *stack, uint64_t *out_value);
int32_t bitstack_peek(const BitStack *stack, uint64_t *out_value);
size_t bitstack_size(const BitStack *stack);
void bitstack_clear(BitStack *stack);

#endif
