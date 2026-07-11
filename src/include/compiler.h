#ifndef _NESTOR_COMPILER_H
#define _NESTOR_COMPILER_H

#include "arena.h"
#include "ast.h"
#include "error_codes.h"

int32_t compile_workflow(Arena *arena, WorkflowAST *ast);

#endif
