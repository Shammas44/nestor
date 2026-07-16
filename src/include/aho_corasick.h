#ifndef _AHO_CORASICK_H
#define _AHO_CORASICK_H

#include "arena.h"
#include "jsonv/jsonv.h"

typedef struct ACNode ACNode;
struct ACNode {
  ACNode *next[256];
  ACNode *fail;
  size_t match_len;
};

/**
 * @brief Build an Aho-Corasick trie using all secrets defined in context_val.
 * @param arena The memory arena to allocate trie nodes.
 * @param context_val The root execution context object containing "secrets".
 * @return The root of the compiled Aho-Corasick trie, or NULL if no secrets are present.
 */
ACNode *ac_create_trie(Arena *arena, Jsonv_Value context_val);

/**
 * @brief Redact secrets from a stream/string using the compiled Aho-Corasick trie.
 * @param root The root of the Aho-Corasick trie.
 * @param input The raw input buffer.
 * @param output The output buffer to write redacted data.
 * @param length The length of the input buffer.
 * @return The length of the redacted output written to the output buffer.
 */
size_t redact_stream(ACNode *root, const char *input, char *output, size_t length);

void set_global_ac_root(ACNode *root);
ACNode *get_global_ac_root(void);

#endif
