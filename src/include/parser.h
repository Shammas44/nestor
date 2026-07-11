#ifndef _NESTOR_PARSER_H
#define _NESTOR_PARSER_H

#include "arena.h"
#include "ast.h"
#include "error_codes.h"

int32_t parser_parse_buffer(Arena *arena, const char *buffer, size_t len, WorkflowAST *out_ast);
int32_t parser_parse_stdin(Arena *arena, WorkflowAST *out_ast);
int32_t parser_parse_file(Arena *arena, const char *filepath, WorkflowAST *out_ast);

#endif
