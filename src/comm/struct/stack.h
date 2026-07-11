#ifndef _NESTOR_STACK_H
#define _NESTOR_STACK_H
#include <stdbool.h>
#include <stdio.h>
#define T Stack

/* Generic value stack */
typedef struct {
  unsigned char *data; /* raw storage */
  unsigned char *extra; /* raw storage */
  size_t elem_size;    /* size of one element */
  int top;             /* index of top element (-1 when empty) */
  size_t capacity;     /* allocated slots */
} T;

/* Lifecycle */
void stack_init(T *s, size_t elem_size, unsigned char *data, size_t data_size);
void stack_destroy(T *s);

/* Operations */
void* stack_push(T *s, const void *elem);
void* stack_pop(T *s);
void* stack_peek(const T *s, size_t index);

/* State */
bool stack_is_empty(const T *s);
size_t stack_size(const T *s);

/* Unsafe direct access helper */
#define STACK_AT(s, type, i) ((type *)((s)->data + ((i) * (s)->elem_size)))

#undef T
#endif
