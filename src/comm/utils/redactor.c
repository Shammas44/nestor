#include "aho_corasick.h"
#include <string.h>

static ACNode *ac_create_node(Arena *arena) {
  /*#region*/
  ACNode *n = na_alloc(arena, sizeof(ACNode));
  if (n) {
    memset(n, 0, sizeof(ACNode));
  }
  return n;
  /*#endregion*/
}

static void ac_insert(Arena *arena, ACNode *root, const char *str, size_t len) {
  /*#region*/
  if (len == 0) return;
  ACNode *curr = root;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)str[i];
    if (!curr->next[c]) {
      curr->next[c] = ac_create_node(arena);
    }
    curr = curr->next[c];
  }
  if (len > curr->match_len) {
    curr->match_len = len;
  }
  /*#endregion*/
}

static void ac_build_failure_links(Arena *arena, ACNode *root) {
  /*#region*/
  typedef struct QueueNode QueueNode;
  struct QueueNode {
    ACNode *ac_node;
    QueueNode *next;
  };

  QueueNode *q_head = NULL;
  QueueNode *q_tail = NULL;

  #define Q_PUSH(node) do { \
    QueueNode *qn = na_alloc(arena, sizeof(QueueNode)); \
    qn->ac_node = (node); \
    qn->next = NULL; \
    if (!q_head) { \
      q_head = qn; \
      q_tail = qn; \
    } else { \
      q_tail->next = qn; \
      q_tail = qn; \
    } \
  } while(0)

  #define Q_POP(out_node) do { \
    out_node = q_head->ac_node; \
    q_head = q_head->next; \
  } while(0)

  for (int c = 0; c < 256; c++) {
    if (root->next[c]) {
      root->next[c]->fail = root;
      Q_PUSH(root->next[c]);
    } else {
      root->next[c] = root;
    }
  }

  while (q_head) {
    ACNode *curr;
    Q_POP(curr);

    for (int c = 0; c < 256; c++) {
      ACNode *child = curr->next[c];
      if (child) {
        ACNode *fail_node = curr->fail;
        while (fail_node != root && !fail_node->next[c]) {
          fail_node = fail_node->fail;
        }
        if (fail_node->next[c] && fail_node->next[c] != child) {
          child->fail = fail_node->next[c];
        } else {
          child->fail = root;
        }

        if (child->fail->match_len > child->match_len) {
          child->match_len = child->fail->match_len;
        }

        Q_PUSH(child);
      } else {
        ACNode *fail_node = curr->fail;
        while (fail_node != root && !fail_node->next[c]) {
          fail_node = fail_node->fail;
        }
        if (fail_node->next[c]) {
          curr->next[c] = fail_node->next[c];
        } else {
          curr->next[c] = root;
        }
      }
    }
  }
  #undef Q_PUSH
  #undef Q_POP
  /*#endregion*/
}

ACNode *ac_create_trie(Arena *arena, Jsonv_Value context_val) {
  /*#region*/
  if (context_val.tag != JSONV_VAL_OBJ) return NULL;

  Jsonv_Value secrets_val;
  if (!jsonv_obj_get(context_val.as.p, "secrets", &secrets_val) || secrets_val.tag != JSONV_VAL_OBJ) {
    return NULL;
  }

  Jsonv_Obj *secrets_obj = secrets_val.as.p;
  int len = jsonv_obj_length(secrets_obj);
  if (len == 0) return NULL;

  ACNode *root = ac_create_node(arena);
  if (!root) return NULL;

  for (int i = 0; i < len; i++) {
    Jsonv_Value val = jsonv_obj_val_at(secrets_obj, i);
    if (val.tag == JSONV_VAL_STRING) {
      const char *secret_str = (const char *)val.as.p;
      size_t secret_len = jsonv_val_str_len(val);
      ac_insert(arena, root, secret_str, secret_len);
    }
  }

  ac_build_failure_links(arena, root);
  return root;
  /*#endregion*/
}

size_t redact_stream(ACNode *root, const char *input, char *output, size_t length) {
  /*#region*/
  if (!input || !output) return 0;
  if (!root || length == 0) {
    if (input != output) {
      memcpy(output, input, length);
    }
    output[length] = '\0';
    return length;
  }

  ACNode *curr = root;
  size_t out_idx = 0;

  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)input[i];
    output[out_idx++] = input[i];
    curr = curr->next[c];

    if (curr->match_len > 0) {
      size_t match_len = curr->match_len;
      if (out_idx >= match_len) {
        out_idx -= match_len;
        output[out_idx++] = '*';
        output[out_idx++] = '*';
        output[out_idx++] = '*';
      }
      curr = root;
    }
  }
  output[out_idx] = '\0';
  return out_idx;
  /*#endregion*/
}

static ACNode *global_ac_root = NULL;

void set_global_ac_root(ACNode *root) {
  /*#region*/
  global_ac_root = root;
  /*#endregion*/
}

ACNode *get_global_ac_root(void) {
  /*#region*/
  return global_ac_root;
  /*#endregion*/
}
