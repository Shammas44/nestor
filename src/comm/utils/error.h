#ifndef _NESTOR_ERROR_H
#define _NESTOR_ERROR_H

typedef enum {
  Memory_Allocation_Failed,
  Assertion_Failed,
  Unknonwn_Token_Encounter
} Except_Type;

typedef struct {
  char description[100];
  const char *path; // Arena-allocated or zero-copy read-only view
  Except_Type type;
} Error;

#endif
