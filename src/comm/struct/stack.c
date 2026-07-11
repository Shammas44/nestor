#include "stack.h"
#include "assert.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>
#define T Stack

void stack_init(T *s, size_t elem_size, unsigned char *data, size_t data_size) {
  /*#region*/
  assert(s);
  assert(data);
  assert(elem_size > 0);
  assert(data_size >= elem_size);
  s->data = data;
  s->extra = NULL;
  s->elem_size = elem_size;
  s->top = -1; /* empty stack */
  s->capacity = data_size / elem_size;
  /*#endregion*/
}

void stack_destroy(T *s) {
  /*#region*/
  if (!s)
    return;
  if (s->extra)
    free(s->extra);
  s->elem_size = 0;
  s->capacity = 0;
  s->top = -1;
  /*#endregion*/
}

static bool stack_grow(T *s) {
  /*#region*/
  size_t new_capacity = s->capacity * 2;
  unsigned char *tmp = CALLOC(new_capacity, s->elem_size);

  if (!tmp)
    return false;

  memcpy(tmp, s->data, s->capacity * s->elem_size);

  if (s->extra) {
    free(s->extra);
  }

  s->extra = tmp;
  s->data = tmp;
  s->capacity = new_capacity;
  return true;
  /*#endregion*/
}

void *stack_push(T *s, const void *elem) {
  /*#region*/
  assert(s);
  assert(elem);

  /* Grow if needed */
  if ((size_t)(s->top + 1) >= s->capacity) {
    if (!stack_grow(s))
      return NULL;
  }

  s->top++;

  memcpy(s->data + (s->top * s->elem_size), elem, s->elem_size);

  return s->data + (s->top * s->elem_size);
  /*#endregion*/
}

void *stack_pop(T *s) {
  /*#region*/
  assert(s);
  assert(s->top >= 0);

  void *p = s->data + (s->top * s->elem_size);

  s->top--;
  return p;
  /*#endregion*/
}

void *stack_peek(const T *s, size_t index) {
  /*#region*/
  assert(s);
  assert(s->top >= 0);
  assert(index <= (size_t)s->top);
  return s->data + (index * s->elem_size);
  /*#endregion*/
}

bool stack_is_empty(const T *s) {
  /*#region*/
  return !s || s->top == -1;
  /*#endregion*/
}

size_t stack_size(const T *s) {
  /*#region*/
  return s ? (size_t)(s->top + 1) : 0;
  /*#endregion*/
}
