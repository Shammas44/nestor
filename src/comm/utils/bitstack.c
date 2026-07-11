#include "bitstack.h"
#include "error_codes.h"

int32_t bitstack_init(BitStack *stack, Arena *arena, size_t capacity) {
  /*#region*/
  if (!stack || !arena)
    return ERR_OOM;

  stack->data = na_alloc(arena, capacity * sizeof(uint64_t));
  if (!stack->data) {
    stack->top = 0;
    stack->capacity = 0;
    return ERR_OOM;
  }

  stack->top = 0;
  stack->capacity = capacity;
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t bitstack_push(BitStack *stack, uint64_t value) {
  /*#region*/
  if (!stack || !stack->data)
    return ERR_OOM;

  // Check for overflow before writing to prevent memory corruption.
  if (stack->top >= stack->capacity) {
    return ERR_OOM;
  }

  stack->data[stack->top] = value;
  stack->top++;
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t bitstack_pop(BitStack *stack, uint64_t *out_value) {
  /*#region*/
  if (!stack || !stack->data)
    return ERR_OOM;

  // Check for underflow.
  if (stack->top == 0) {
    return ERR_OOM;
  }

  stack->top--;
  if (out_value) {
    *out_value = stack->data[stack->top];
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t bitstack_peek(const BitStack *stack, uint64_t *out_value) {
  /*#region*/
  if (!stack || !stack->data)
    return ERR_OOM;

  // Check if stack is empty.
  if (stack->top == 0) {
    return ERR_OOM;
  }

  if (out_value) {
    // Look at top element without modifying the stack pointer.
    *out_value = stack->data[stack->top - 1];
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

size_t bitstack_size(const BitStack *stack) {
  /*#region*/
  if (!stack)
    return 0;
  return stack->top;
  /*#endregion*/
}

void bitstack_clear(BitStack *stack) {
  /*#region*/
  if (stack) {
    stack->top = 0;
  }
  /*#endregion*/
}
