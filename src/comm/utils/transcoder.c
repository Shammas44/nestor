#include "types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char *allocate_jsonv_string_from_sv(Arena *arena, StringView sv) {
  /*#region*/
  size_t total_size = sizeof(uint32_t) + sv.length + 1;
  char *buf = (char *)na_alloc(arena, total_size);
  if (!buf) return NULL;
  *(uint32_t *)buf = (uint32_t)sv.length;
  char *str_ptr = buf + sizeof(uint32_t);
  if (sv.length > 0 && sv.data) {
    memcpy(str_ptr, sv.data, sv.length);
  }
  str_ptr[sv.length] = '\0';
  return str_ptr;
  /*#endregion*/
}

static char *percent_decode(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  char *decoded = na_alloc(arena, len + sizeof(uint32_t) + 1);
  if (!decoded) return NULL;
  char *write_ptr = decoded + sizeof(uint32_t);
  size_t i = 0;
  size_t d_len = 0;
  while (i < len) {
    if (data[i] == '%') {
      if (i + 2 < len) {
        char h1 = data[i+1];
        char h2 = data[i+2];
        int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
                 (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) :
                 (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) : -1;
        int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
                 (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) :
                 (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) : -1;
        if (v1 >= 0 && v2 >= 0) {
          write_ptr[d_len++] = (char)((v1 << 4) | v2);
          i += 3;
          continue;
        }
      }
    }
    if (data[i] == '+') {
      write_ptr[d_len++] = ' ';
    } else {
      write_ptr[d_len++] = data[i];
    }
    i++;
  }
  write_ptr[d_len] = '\0';
  *(uint32_t *)decoded = (uint32_t)d_len;
  *out_len = d_len;
  return write_ptr;
  /*#endregion*/
}

static char *percent_encode_str(Arena *arena, const char *str, size_t len, size_t *out_len) {
  /*#region*/
  size_t cap = 3 * len + 1;
  char *buf = na_alloc(arena, cap);
  if (!buf) return NULL;
  size_t d_len = 0;
  for (size_t i = 0; i < len; i++) {
    char c = str[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      buf[d_len++] = c;
    } else if (c == ' ') {
      buf[d_len++] = '+';
    } else {
      sprintf(buf + d_len, "%%%02X", (unsigned char)c);
      d_len += 3;
    }
  }
  buf[d_len] = '\0';
  *out_len = d_len;
  return buf;
  /*#endregion*/
}

static char *base64_encode(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t elen = 4 * ((len + 2) / 3);
  char *buf = na_alloc(arena, elen + 1);
  if (!buf) return NULL;
  size_t i = 0, j = 0;
  while (i < len) {
    uint32_t octet_a = (unsigned char)data[i++];
    uint32_t octet_b = (i < len) ? (unsigned char)data[i++] : 0;
    uint32_t octet_c = (i < len) ? (unsigned char)data[i++] : 0;
    uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
    buf[j++] = table[(triple >> 18) & 0x3F];
    buf[j++] = table[(triple >> 12) & 0x3F];
    buf[j++] = (i > len + 1) ? '=' : table[(triple >> 6) & 0x3F];
    buf[j++] = (i > len) ? '=' : table[triple & 0x3F];
  }
  buf[elen] = '\0';
  *out_len = elen;
  return buf;
  /*#endregion*/
}

#define DECODE_B64_CHAR(c) \
    ((c) >= 'A' && (c) <= 'Z' ? (c) - 'A' : \
     (c) >= 'a' && (c) <= 'z' ? (c) - 'a' + 26 : \
     (c) >= '0' && (c) <= '9' ? (c) - '0' + 52 : \
     (c) == '+' ? 62 : \
     (c) == '/' ? 63 : -1)

static char *base64_decode(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  if (len % 4 != 0) return NULL;
  size_t dlen = (len / 4) * 3;
  if (len > 0) {
    if (data[len - 1] == '=') dlen--;
    if (data[len - 2] == '=') dlen--;
  }
  char *buf = na_alloc(arena, dlen + sizeof(uint32_t) + 1);
  if (!buf) return NULL;
  char *write_ptr = buf + sizeof(uint32_t);
  
  size_t i = 0, j = 0;
  while (i < len) {
    int c1 = DECODE_B64_CHAR(data[i]);
    int c2 = DECODE_B64_CHAR(data[i+1]);
    int c3 = (data[i+2] == '=') ? 0 : DECODE_B64_CHAR(data[i+2]);
    int c4 = (data[i+3] == '=') ? 0 : DECODE_B64_CHAR(data[i+3]);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0) return NULL;
    uint32_t triple = (c1 << 18) | (c2 << 12) | (c3 << 6) | c4;
    if (j < dlen) write_ptr[j++] = (char)((triple >> 16) & 0xFF);
    if (j < dlen) write_ptr[j++] = (char)((triple >> 8) & 0xFF);
    if (j < dlen) write_ptr[j++] = (char)(triple & 0xFF);
    i += 4;
  }
  write_ptr[dlen] = '\0';
  *(uint32_t *)buf = (uint32_t)dlen;
  *out_len = dlen;
  return write_ptr;
  /*#endregion*/
}

static char *hex_encode(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  size_t elen = 2 * len;
  char *buf = na_alloc(arena, elen + 1);
  if (!buf) return NULL;
  for (size_t i = 0; i < len; i++) {
    sprintf(buf + 2 * i, "%02x", (unsigned char)data[i]);
  }
  buf[elen] = '\0';
  *out_len = elen;
  return buf;
  /*#endregion*/
}

static char *hex_decode(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  if (len % 2 != 0) return NULL;
  size_t dlen = len / 2;
  char *buf = na_alloc(arena, dlen + sizeof(uint32_t) + 1);
  if (!buf) return NULL;
  char *write_ptr = buf + sizeof(uint32_t);
  for (size_t i = 0; i < dlen; i++) {
    char h1 = data[2 * i];
    char h2 = data[2 * i + 1];
    int v1 = (h1 >= '0' && h1 <= '9') ? (h1 - '0') :
             (h1 >= 'A' && h1 <= 'F') ? (h1 - 'A' + 10) :
             (h1 >= 'a' && h1 <= 'f') ? (h1 - 'a' + 10) : -1;
    int v2 = (h2 >= '0' && h2 <= '9') ? (h2 - '0') :
             (h2 >= 'A' && h2 <= 'F') ? (h2 - 'A' + 10) :
             (h2 >= 'a' && h2 <= 'f') ? (h2 - 'a' + 10) : -1;
    if (v1 < 0 || v2 < 0) return NULL;
    write_ptr[i] = (char)((v1 << 4) | v2);
  }
  write_ptr[dlen] = '\0';
  *(uint32_t *)buf = (uint32_t)dlen;
  *out_len = dlen;
  return write_ptr;
  /*#endregion*/
}

static int32_t parse_csv_row(Arena *arena, const char *data, size_t len, size_t *pos, char delimiter, bool relaxed, StringView *fields, size_t max_fields, size_t *out_field_count) {
  /*#region*/
  size_t col = 0;
  size_t p = *pos;
  
  while (p < len && (data[p] == '\r' || data[p] == '\n')) {
    p++;
  }
  if (p >= len) {
    *pos = p;
    *out_field_count = 0;
    return ERR_SUCCESS;
  }

  while (p < len && data[p] != '\r' && data[p] != '\n') {
    if (col >= max_fields) {
      if (relaxed) {
        while (p < len && data[p] != '\r' && data[p] != '\n') p++;
        break;
      } else {
        return ERR_TRANSCODE;
      }
    }
    
    if (data[p] == '"') {
      p++;
      size_t start = p;
      bool has_escaped_quotes = false;
      while (p < len) {
        if (data[p] == '"') {
          if (p + 1 < len && data[p+1] == '"') {
            has_escaped_quotes = true;
            p += 2;
          } else {
            p++;
            break;
          }
        } else {
          p++;
        }
      }
      size_t end = p - 1;
      
      if (has_escaped_quotes) {
        size_t raw_len = end - start;
        char *unescaped = na_alloc(arena, raw_len + 1);
        if (!unescaped) return ERR_OOM;
        size_t u_idx = 0;
        for (size_t k = start; k < end; k++) {
          if (data[k] == '"' && k + 1 < end && data[k+1] == '"') {
            unescaped[u_idx++] = '"';
            k++;
          } else {
            unescaped[u_idx++] = data[k];
          }
        }
        unescaped[u_idx] = '\0';
        fields[col++] = (StringView){ unescaped, u_idx };
      } else {
        fields[col++] = (StringView){ data + start, end - start };
      }
      
      while (p < len && data[p] != delimiter && data[p] != '\r' && data[p] != '\n') {
        p++;
      }
    } else {
      size_t start = p;
      while (p < len && data[p] != delimiter && data[p] != '\r' && data[p] != '\n') {
        p++;
      }
      fields[col++] = (StringView){ data + start, p - start };
    }
    
    if (p < len && data[p] == delimiter) {
      p++;
      if (p >= len || data[p] == '\r' || data[p] == '\n') {
        if (col < max_fields) {
          fields[col++] = (StringView){ "", 0 };
        }
      }
    }
  }
  
  while (p < len && (data[p] == '\r' || data[p] == '\n')) {
    p++;
  }
  
  *pos = p;
  *out_field_count = col;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t write_csv_cell(Arena *arena, char **buf_ptr, size_t *buf_len_ptr, size_t *buf_cap_ptr, char delim, const char *cell_str, size_t cell_len) {
  /*#region*/
  bool need_quotes = false;
  size_t escaped_len = cell_len;
  for (size_t k = 0; k < cell_len; k++) {
    if (cell_str[k] == delim || cell_str[k] == '"' || cell_str[k] == '\n' || cell_str[k] == '\r') {
      need_quotes = true;
    }
    if (cell_str[k] == '"') {
      escaped_len++;
    }
  }
  size_t needed = need_quotes ? (escaped_len + 2) : cell_len;
  size_t buf_len = *buf_len_ptr;
  size_t buf_cap = *buf_cap_ptr;
  char *buf = *buf_ptr;
  if (buf_len + needed >= buf_cap) {
    buf_cap += needed + 1024;
    char *new_buf = na_alloc(arena, buf_cap);
    if (!new_buf) return ERR_OOM;
    memcpy(new_buf, buf, buf_len);
    buf = new_buf;
    *buf_ptr = buf;
    *buf_cap_ptr = buf_cap;
  }
  if (need_quotes) {
    buf[buf_len++] = '"';
    for (size_t k = 0; k < cell_len; k++) {
      if (cell_str[k] == '"') {
        buf[buf_len++] = '"';
        buf[buf_len++] = '"';
      } else {
        buf[buf_len++] = cell_str[k];
      }
    }
    buf[buf_len++] = '"';
  } else {
    memcpy(buf + buf_len, cell_str, cell_len);
    buf_len += cell_len;
  }
  *buf_len_ptr = buf_len;
  return ERR_SUCCESS;
  /*#endregion*/
}

typedef struct {
  const char *data;
  size_t len;
  size_t pos;
} XmlParser;

static void xml_skip_ws(XmlParser *parser) {
  /*#region*/
  while (parser->pos < parser->len) {
    char c = parser->data[parser->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      parser->pos++;
    } else {
      break;
    }
  }
  /*#endregion*/
}

static StringView xml_parse_name(XmlParser *parser) {
  /*#region*/
  xml_skip_ws(parser);
  size_t start = parser->pos;
  while (parser->pos < parser->len) {
    char c = parser->data[parser->pos];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == ':' || c == '.') {
      parser->pos++;
    } else {
      break;
    }
  }
  return (StringView){ parser->data + start, parser->pos - start };
  /*#endregion*/
}

static StringView xml_parse_attr_value(XmlParser *parser) {
  /*#region*/
  xml_skip_ws(parser);
  if (parser->pos >= parser->len) return (StringView){"", 0};
  char quote = parser->data[parser->pos];
  if (quote != '"' && quote != '\'') return (StringView){"", 0};
  parser->pos++;
  size_t start = parser->pos;
  while (parser->pos < parser->len && parser->data[parser->pos] != quote) {
    parser->pos++;
  }
  size_t end = parser->pos;
  if (parser->pos < parser->len) parser->pos++;
  return (StringView){ parser->data + start, end - start };
  /*#endregion*/
}

static char *xml_unescape_text(Arena *arena, const char *data, size_t len, size_t *out_len) {
  /*#region*/
  char *buf = na_alloc(arena, len + sizeof(uint32_t) + 1);
  if (!buf) return NULL;
  char *write_ptr = buf + sizeof(uint32_t);
  size_t i = 0;
  size_t d_len = 0;
  while (i < len) {
    if (data[i] == '&') {
      if (i + 4 <= len && strncmp(data + i, "&lt;", 4) == 0) {
        write_ptr[d_len++] = '<'; i += 4; continue;
      } else if (i + 4 <= len && strncmp(data + i, "&gt;", 4) == 0) {
        write_ptr[d_len++] = '>'; i += 4; continue;
      } else if (i + 5 <= len && strncmp(data + i, "&amp;", 5) == 0) {
        write_ptr[d_len++] = '&'; i += 5; continue;
      } else if (i + 6 <= len && strncmp(data + i, "&quot;", 6) == 0) {
        write_ptr[d_len++] = '"'; i += 6; continue;
      } else if (i + 6 <= len && strncmp(data + i, "&apos;", 6) == 0) {
        write_ptr[d_len++] = '\''; i += 6; continue;
      }
    }
    write_ptr[d_len++] = data[i++];
  }
  write_ptr[d_len] = '\0';
  *(uint32_t *)buf = (uint32_t)d_len;
  *out_len = d_len;
  return write_ptr;
  /*#endregion*/
}

typedef struct {
  StringView name;
  StringView value;
} XmlAttr;

typedef struct XmlNode XmlNode;
struct XmlNode {
  StringView tag;
  XmlAttr attrs[32];
  size_t attr_count;
  XmlNode *children[128];
  size_t child_count;
  StringView text_content;
  bool is_self_closing;
};

static XmlNode *parse_xml_element(Arena *arena, XmlParser *parser) {
  /*#region*/
  xml_skip_ws(parser);
  if (parser->pos >= parser->len || parser->data[parser->pos] != '<') return NULL;
  parser->pos++;
  
  if (parser->pos < parser->len && parser->data[parser->pos] == '!') {
    while (parser->pos < parser->len && parser->data[parser->pos] != '>') parser->pos++;
    if (parser->pos < parser->len) parser->pos++;
    return NULL;
  }
  if (parser->pos < parser->len && parser->data[parser->pos] == '?') {
    while (parser->pos < parser->len && !(parser->data[parser->pos] == '?' && parser->pos + 1 < parser->len && parser->data[parser->pos + 1] == '>')) {
      parser->pos++;
    }
    parser->pos += 2;
    return NULL;
  }

  if (parser->pos < parser->len && parser->data[parser->pos] == '/') {
    return NULL;
  }

  XmlNode *node = na_alloc(arena, sizeof(XmlNode));
  if (!node) return NULL;
  memset(node, 0, sizeof(XmlNode));

  node->tag = xml_parse_name(parser);
  if (node->tag.length == 0) return NULL;

  while (parser->pos < parser->len) {
    xml_skip_ws(parser);
    if (parser->pos >= parser->len) break;
    if (parser->data[parser->pos] == '>') {
      parser->pos++;
      break;
    }
    if (parser->data[parser->pos] == '/' && parser->pos + 1 < parser->len && parser->data[parser->pos + 1] == '>') {
      node->is_self_closing = true;
      parser->pos += 2;
      break;
    }
    
    StringView attr_name = xml_parse_name(parser);
    if (attr_name.length == 0) break;
    xml_skip_ws(parser);
    if (parser->pos < parser->len && parser->data[parser->pos] == '=') {
      parser->pos++;
      StringView attr_val = xml_parse_attr_value(parser);
      if (node->attr_count < 32) {
        node->attrs[node->attr_count].name = attr_name;
        node->attrs[node->attr_count].value = attr_val;
        node->attr_count++;
      }
    }
  }

  if (node->is_self_closing) {
    return node;
  }

  size_t text_start = parser->pos;
  bool has_text = false;
  while (parser->pos < parser->len) {
    xml_skip_ws(parser);
    if (parser->pos >= parser->len) break;
    
    if (parser->data[parser->pos] == '<') {
      if (parser->pos + 1 < parser->len && parser->data[parser->pos + 1] == '/') {
        size_t text_end = parser->pos;
        if (!has_text || node->child_count > 0) {
          // If we had text but then children, or just empty
        } else {
          node->text_content = (StringView){ parser->data + text_start, text_end - text_start };
        }
        parser->pos += 2;
        xml_parse_name(parser);
        xml_skip_ws(parser);
        if (parser->pos < parser->len && parser->data[parser->pos] == '>') parser->pos++;
        break;
      } else if (parser->pos + 9 < parser->len && strncmp(parser->data + parser->pos, "<![CDATA[", 9) == 0) {
        parser->pos += 9;
        size_t cdata_start = parser->pos;
        while (parser->pos + 2 < parser->len && strncmp(parser->data + parser->pos, "]]>", 3) != 0) {
          parser->pos++;
        }
        size_t cdata_end = parser->pos;
        parser->pos += 3;
        node->text_content = (StringView){ parser->data + cdata_start, cdata_end - cdata_start };
        has_text = true;
        text_start = parser->pos;
      } else {
        XmlNode *child = parse_xml_element(arena, parser);
        if (child) {
          if (node->child_count < 128) {
            node->children[node->child_count++] = child;
          }
        }
      }
    } else {
      parser->pos++;
      has_text = true;
    }
  }
  
  if (!has_text && node->child_count == 0) {
    node->text_content = (StringView){ "", 0 };
  } else if (node->child_count > 0) {
    // ignore text in mixed
  } else {
    const char *t_data = node->text_content.data;
    size_t t_len = node->text_content.length;
    while (t_len > 0 && (t_data[0] == ' ' || t_data[0] == '\t' || t_data[0] == '\n' || t_data[0] == '\r')) {
      t_data++;
      t_len--;
    }
    while (t_len > 0 && (t_data[t_len - 1] == ' ' || t_data[t_len - 1] == '\t' || t_data[t_len - 1] == '\n' || t_data[t_len - 1] == '\r')) {
      t_len--;
    }
    node->text_content = (StringView){ t_data, t_len };
  }

  return node;
  /*#endregion*/
}

static Jsonv_Value xml_to_json_parker(Arena *arena, Jsonv_Arena *jsonv_arena, XmlNode *node) {
  /*#region*/
  if (node->child_count == 0) {
    size_t u_len = 0;
    char *str = xml_unescape_text(arena, node->text_content.data, node->text_content.length, &u_len);
    if (!str) return jsonv_val_undefined();
    return jsonv_val_str(str);
  }

  Jsonv_Obj *obj = jsonv_obj_new(jsonv_arena, NULL);
  if (!obj) return jsonv_val_undefined();

  for (size_t i = 0; i < node->child_count; i++) {
    XmlNode *child = node->children[i];
    char *key = allocate_jsonv_string_from_sv(arena, child->tag);
    if (!key) return jsonv_val_undefined();

    bool repeated = false;
    size_t first_idx = i;
    for (size_t j = i + 1; j < node->child_count; j++) {
      if (node->children[j]->tag.length == child->tag.length &&
          strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
        repeated = true;
        break;
      }
    }

    if (repeated) {
      Jsonv_Arr *arr = jsonv_arr_new(jsonv_arena);
      if (!arr) return jsonv_val_undefined();
      int arr_idx = 0;
      for (size_t j = first_idx; j < node->child_count; j++) {
        if (node->children[j]->tag.length == child->tag.length &&
            strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
          Jsonv_Value val = xml_to_json_parker(arena, jsonv_arena, node->children[j]);
          jsonv_arr_set(jsonv_arena, arr, arr_idx++, val);
        }
      }
      jsonv_obj_set(jsonv_arena, obj, key, jsonv_val_arr(arr));
      size_t last_idx = first_idx;
      for (size_t j = first_idx + 1; j < node->child_count; j++) {
        if (node->children[j]->tag.length == child->tag.length &&
            strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
          last_idx = j;
        }
      }
      i = last_idx;
    } else {
      Jsonv_Value val = xml_to_json_parker(arena, jsonv_arena, child);
      jsonv_obj_set(jsonv_arena, obj, key, val);
    }
  }
  return jsonv_val_obj(obj);
  /*#endregion*/
}

static Jsonv_Value xml_to_json_badgerfish(Arena *arena, Jsonv_Arena *jsonv_arena, XmlNode *node) {
  /*#region*/
  Jsonv_Obj *obj = jsonv_obj_new(jsonv_arena, NULL);
  if (!obj) return jsonv_val_undefined();

  for (size_t i = 0; i < node->attr_count; i++) {
    size_t k_len = node->attrs[i].name.length + 1;
    char *k_buf = na_alloc(arena, k_len + sizeof(uint32_t) + 1);
    if (!k_buf) return jsonv_val_undefined();
    *(uint32_t *)k_buf = (uint32_t)k_len;
    char *k_str = k_buf + sizeof(uint32_t);
    k_str[0] = '@';
    memcpy(k_str + 1, node->attrs[i].name.data, node->attrs[i].name.length);
    k_str[k_len] = '\0';

    size_t u_len = 0;
    char *v_str = xml_unescape_text(arena, node->attrs[i].value.data, node->attrs[i].value.length, &u_len);
    if (!v_str) return jsonv_val_undefined();

    jsonv_obj_set(jsonv_arena, obj, k_str, jsonv_val_str(v_str));
  }

  for (size_t i = 0; i < node->child_count; i++) {
    XmlNode *child = node->children[i];
    char *key = allocate_jsonv_string_from_sv(arena, child->tag);
    if (!key) return jsonv_val_undefined();

    bool repeated = false;
    size_t first_idx = i;
    for (size_t j = i + 1; j < node->child_count; j++) {
      if (node->children[j]->tag.length == child->tag.length &&
          strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
        repeated = true;
        break;
      }
    }

    if (repeated) {
      Jsonv_Arr *arr = jsonv_arr_new(jsonv_arena);
      if (!arr) return jsonv_val_undefined();
      int arr_idx = 0;
      for (size_t j = first_idx; j < node->child_count; j++) {
        if (node->children[j]->tag.length == child->tag.length &&
            strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
          Jsonv_Value val = xml_to_json_badgerfish(arena, jsonv_arena, node->children[j]);
          jsonv_arr_set(jsonv_arena, arr, arr_idx++, val);
        }
      }
      jsonv_obj_set(jsonv_arena, obj, key, jsonv_val_arr(arr));
      size_t last_idx = first_idx;
      for (size_t j = first_idx + 1; j < node->child_count; j++) {
        if (node->children[j]->tag.length == child->tag.length &&
            strncmp(node->children[j]->tag.data, child->tag.data, child->tag.length) == 0) {
          last_idx = j;
        }
      }
      i = last_idx;
    } else {
      Jsonv_Value val = xml_to_json_badgerfish(arena, jsonv_arena, child);
      jsonv_obj_set(jsonv_arena, obj, key, val);
    }
  }

  if (node->text_content.length > 0 || (node->child_count == 0 && node->attr_count == 0)) {
    size_t u_len = 0;
    char *str = xml_unescape_text(arena, node->text_content.data, node->text_content.length, &u_len);
    if (!str) return jsonv_val_undefined();
    
    char *t_key = allocate_jsonv_string_from_sv(arena, (StringView){ "$", 1 });
    jsonv_obj_set(jsonv_arena, obj, t_key, jsonv_val_str(str));
  }

  return jsonv_val_obj(obj);
  /*#endregion*/
}

static Jsonv_Value xml_to_json_jsonml(Arena *arena, Jsonv_Arena *jsonv_arena, XmlNode *node) {
  /*#region*/
  Jsonv_Arr *arr = jsonv_arr_new(jsonv_arena);
  if (!arr) return jsonv_val_undefined();
  int arr_idx = 0;

  char *tag_str = allocate_jsonv_string_from_sv(arena, node->tag);
  if (!tag_str) return jsonv_val_undefined();
  jsonv_arr_set(jsonv_arena, arr, arr_idx++, jsonv_val_str(tag_str));

  if (node->attr_count > 0) {
    Jsonv_Obj *attr_obj = jsonv_obj_new(jsonv_arena, NULL);
    if (!attr_obj) return jsonv_val_undefined();
    for (size_t i = 0; i < node->attr_count; i++) {
      char *k = allocate_jsonv_string_from_sv(arena, node->attrs[i].name);
      size_t u_len = 0;
      char *v = xml_unescape_text(arena, node->attrs[i].value.data, node->attrs[i].value.length, &u_len);
      if (!k || !v) return jsonv_val_undefined();
      jsonv_obj_set(jsonv_arena, attr_obj, k, jsonv_val_str(v));
    }
    jsonv_arr_set(jsonv_arena, arr, arr_idx++, jsonv_val_obj(attr_obj));
  }

  if (node->child_count > 0) {
    for (size_t i = 0; i < node->child_count; i++) {
      Jsonv_Value child_val = xml_to_json_jsonml(arena, jsonv_arena, node->children[i]);
      jsonv_arr_set(jsonv_arena, arr, arr_idx++, child_val);
    }
  } else if (node->text_content.length > 0) {
    size_t u_len = 0;
    char *str = xml_unescape_text(arena, node->text_content.data, node->text_content.length, &u_len);
    if (!str) return jsonv_val_undefined();
    jsonv_arr_set(jsonv_arena, arr, arr_idx++, jsonv_val_str(str));
  }

  return jsonv_val_arr(arr);
  /*#endregion*/
}

static int32_t xml_write_str(Arena *arena, char **buf_ptr, size_t *buf_len_ptr, size_t *buf_cap_ptr, const char *s, size_t len) {
  /*#region*/
  size_t buf_len = *buf_len_ptr;
  size_t buf_cap = *buf_cap_ptr;
  char *buf = *buf_ptr;
  if (buf_len + len >= buf_cap) {
    buf_cap += len + 1024;
    char *new_buf = na_alloc(arena, buf_cap);
    if (!new_buf) return ERR_OOM;
    memcpy(new_buf, buf, buf_len);
    buf = new_buf;
    *buf_ptr = buf;
    *buf_cap_ptr = buf_cap;
  }
  memcpy(buf + buf_len, s, len);
  *buf_len_ptr = buf_len + len;
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t xml_write_escaped(Arena *arena, char **buf_ptr, size_t *buf_len_ptr, size_t *buf_cap_ptr, const char *s, size_t len) {
  /*#region*/
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    int32_t status = ERR_SUCCESS;
    if (c == '<') status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, "&lt;", 4);
    else if (c == '>') status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, "&gt;", 4);
    else if (c == '&') status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, "&amp;", 5);
    else if (c == '"') status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, "&quot;", 6);
    else if (c == '\'') status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, "&apos;", 6);
    else {
      char tmp[1] = { c };
      status = xml_write_str(arena, buf_ptr, buf_len_ptr, buf_cap_ptr, tmp, 1);
    }
    if (status != ERR_SUCCESS) return status;
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

static int32_t json_to_xml_helper(Arena *arena, char **buf_ptr, size_t *buf_len, size_t *buf_cap, Jsonv_Value val, StringView tag, XmlConvention conv) {
  /*#region*/
  int32_t status = ERR_SUCCESS;

  if (conv == XML_PARKER) {
    if (val.tag == JSONV_VAL_STRING) {
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "<", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_escaped(arena, buf_ptr, buf_len, buf_cap, (const char *)val.as.p, jsonv_val_str_len(val)); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "</", 2); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
    } else if (val.tag == JSONV_VAL_INT || val.tag == JSONV_VAL_DOUBLE || val.tag == JSONV_VAL_BOOLEAN || val.tag == JSONV_VAL_NULL) {
      char tmp[64];
      size_t t_len = 0;
      if (val.tag == JSONV_VAL_INT) t_len = snprintf(tmp, sizeof(tmp), "%lld", (long long)val.as.i);
      else if (val.tag == JSONV_VAL_DOUBLE) t_len = snprintf(tmp, sizeof(tmp), "%g", val.as.d);
      else if (val.tag == JSONV_VAL_BOOLEAN) { strcpy(tmp, val.as.boolean ? "true" : "false"); t_len = strlen(tmp); }
      else { strcpy(tmp, "null"); t_len = 4; }
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "<", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tmp, t_len); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "</", 2); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
    } else if (val.tag == JSONV_VAL_OBJ) {
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "<", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
      Jsonv_Obj *obj = val.as.p;
      int count = jsonv_obj_length(obj);
      for (int i = 0; i < count; i++) {
        const char *k = jsonv_obj_key_at(obj, i);
        Jsonv_Value v = jsonv_obj_val_at(obj, i);
        StringView child_tag = { k, strlen(k) };
        if (v.tag == JSONV_VAL_ARRAY) {
          Jsonv_Arr *arr = v.as.p;
          int arr_len = jsonv_arr_length(arr);
          for (int j = 0; j < arr_len; j++) {
            Jsonv_Value item;
            if (jsonv_arr_get(arr, j, &item)) {
              status = json_to_xml_helper(arena, buf_ptr, buf_len, buf_cap, item, child_tag, conv);
              if (status != ERR_SUCCESS) return status;
            }
          }
        } else {
          status = json_to_xml_helper(arena, buf_ptr, buf_len, buf_cap, v, child_tag, conv);
          if (status != ERR_SUCCESS) return status;
        }
      }
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "</", 2); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
    }
  } else if (conv == XML_BADGERFISH) {
    if (val.tag == JSONV_VAL_OBJ) {
      Jsonv_Obj *obj = val.as.p;
      int count = jsonv_obj_length(obj);
      
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "<", 1); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      
      for (int i = 0; i < count; i++) {
        const char *k = jsonv_obj_key_at(obj, i);
        if (k[0] == '@') {
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, " ", 1); if (status != ERR_SUCCESS) return status;
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, k + 1, strlen(k) - 1); if (status != ERR_SUCCESS) return status;
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "=\"", 2); if (status != ERR_SUCCESS) return status;
          Jsonv_Value attr_val = jsonv_obj_val_at(obj, i);
          if (attr_val.tag == JSONV_VAL_STRING) {
            status = xml_write_escaped(arena, buf_ptr, buf_len, buf_cap, (const char *)attr_val.as.p, jsonv_val_str_len(attr_val));
            if (status != ERR_SUCCESS) return status;
          }
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "\"", 1); if (status != ERR_SUCCESS) return status;
        }
      }
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;

      for (int i = 0; i < count; i++) {
        const char *k = jsonv_obj_key_at(obj, i);
        if (k[0] == '@') continue;
        Jsonv_Value v = jsonv_obj_val_at(obj, i);
        if (strcmp(k, "$") == 0) {
          if (v.tag == JSONV_VAL_STRING) {
            status = xml_write_escaped(arena, buf_ptr, buf_len, buf_cap, (const char *)v.as.p, jsonv_val_str_len(v));
            if (status != ERR_SUCCESS) return status;
          }
        } else {
          StringView child_tag = { k, strlen(k) };
          if (v.tag == JSONV_VAL_ARRAY) {
            Jsonv_Arr *arr = v.as.p;
            int arr_len = jsonv_arr_length(arr);
            for (int j = 0; j < arr_len; j++) {
              Jsonv_Value item;
              if (jsonv_arr_get(arr, j, &item)) {
                status = json_to_xml_helper(arena, buf_ptr, buf_len, buf_cap, item, child_tag, conv);
                if (status != ERR_SUCCESS) return status;
              }
            }
          } else {
            status = json_to_xml_helper(arena, buf_ptr, buf_len, buf_cap, v, child_tag, conv);
            if (status != ERR_SUCCESS) return status;
          }
        }
      }
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "</", 2); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag.data, tag.length); if (status != ERR_SUCCESS) return status;
      status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
    }
  } else if (conv == XML_JSONML) {
    if (val.tag == JSONV_VAL_ARRAY) {
      Jsonv_Arr *arr = val.as.p;
      int arr_len = jsonv_arr_length(arr);
      if (arr_len > 0) {
        Jsonv_Value tag_val;
        if (jsonv_arr_get(arr, 0, &tag_val) && tag_val.tag == JSONV_VAL_STRING) {
          const char *tag_name = (const char *)tag_val.as.p;
          size_t tag_name_len = jsonv_val_str_len(tag_val);
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "<", 1); if (status != ERR_SUCCESS) return status;
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag_name, tag_name_len); if (status != ERR_SUCCESS) return status;

          int start_child = 1;
          if (arr_len > 1) {
            Jsonv_Value second_val;
            if (jsonv_arr_get(arr, 1, &second_val) && second_val.tag == JSONV_VAL_OBJ) {
              start_child = 2;
              Jsonv_Obj *attr_obj = second_val.as.p;
              int attr_count = jsonv_obj_length(attr_obj);
              for (int i = 0; i < attr_count; i++) {
                const char *k = jsonv_obj_key_at(attr_obj, i);
                status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, " ", 1); if (status != ERR_SUCCESS) return status;
                status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, k, strlen(k)); if (status != ERR_SUCCESS) return status;
                status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "=\"", 2); if (status != ERR_SUCCESS) return status;
                Jsonv_Value attr_val = jsonv_obj_val_at(attr_obj, i);
                if (attr_val.tag == JSONV_VAL_STRING) {
                  status = xml_write_escaped(arena, buf_ptr, buf_len, buf_cap, (const char *)attr_val.as.p, jsonv_val_str_len(attr_val));
                  if (status != ERR_SUCCESS) return status;
                }
                status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "\"", 1); if (status != ERR_SUCCESS) return status;
              }
            }
          }
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;

          for (int i = start_child; i < arr_len; i++) {
            Jsonv_Value child;
            if (jsonv_arr_get(arr, i, &child)) {
              if (child.tag == JSONV_VAL_STRING) {
                status = xml_write_escaped(arena, buf_ptr, buf_len, buf_cap, (const char *)child.as.p, jsonv_val_str_len(child));
                if (status != ERR_SUCCESS) return status;
              } else if (child.tag == JSONV_VAL_ARRAY) {
                status = json_to_xml_helper(arena, buf_ptr, buf_len, buf_cap, child, (StringView){NULL,0}, conv);
                if (status != ERR_SUCCESS) return status;
              }
            }
          }

          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, "</", 2); if (status != ERR_SUCCESS) return status;
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, tag_name, tag_name_len); if (status != ERR_SUCCESS) return status;
          status = xml_write_str(arena, buf_ptr, buf_len, buf_cap, ">", 1); if (status != ERR_SUCCESS) return status;
        }
      }
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t csv_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, CsvOptions opts, Jsonv_Value *out_val) {
  /*#region*/
  if (!out_val) return ERR_INVALID_BOUNDARY;
  char delim = opts.delimiter == '\0' ? ',' : opts.delimiter;

  size_t max_cols = 1024;
  StringView *headers = na_alloc(arena, max_cols * sizeof(StringView));
  if (!headers) return ERR_OOM;
  size_t header_count = 0;

  size_t pos = 0;
  int32_t status;
  
  if (opts.header) {
    status = parse_csv_row(arena, data.data, data.length, &pos, delim, opts.relaxed, headers, max_cols, &header_count);
    if (status != ERR_SUCCESS) return status;
    if (header_count == 0) {
      *out_val = jsonv_val_arr(jsonv_arr_new(jsonv_arena));
      return ERR_SUCCESS;
    }
  }

  Jsonv_Arr *rows_arr = jsonv_arr_new(jsonv_arena);
  if (!rows_arr) return ERR_OOM;
  int row_idx = 0;

  StringView *row_fields = na_alloc(arena, max_cols * sizeof(StringView));
  if (!row_fields) return ERR_OOM;

  while (pos < data.length) {
    size_t field_count = 0;
    status = parse_csv_row(arena, data.data, data.length, &pos, delim, opts.relaxed, row_fields, max_cols, &field_count);
    if (status != ERR_SUCCESS) return status;
    if (field_count == 0) continue;

    if (opts.header) {
      Jsonv_Obj *row_obj = jsonv_obj_new(jsonv_arena, NULL);
      if (!row_obj) return ERR_OOM;
      size_t cols = field_count < header_count ? field_count : header_count;
      if (!opts.relaxed && field_count != header_count) {
        return ERR_TRANSCODE;
      }
      for (size_t col = 0; col < cols; col++) {
        char *key = allocate_jsonv_string_from_sv(arena, headers[col]);
        char *val_str = allocate_jsonv_string_from_sv(arena, row_fields[col]);
        if (!key || !val_str) return ERR_OOM;
        jsonv_obj_set(jsonv_arena, row_obj, key, jsonv_val_str(val_str));
      }
      jsonv_arr_set(jsonv_arena, rows_arr, row_idx++, jsonv_val_obj(row_obj));
    } else {
      Jsonv_Arr *row_arr = jsonv_arr_new(jsonv_arena);
      if (!row_arr) return ERR_OOM;
      for (size_t col = 0; col < field_count; col++) {
        char *val_str = allocate_jsonv_string_from_sv(arena, row_fields[col]);
        if (!val_str) return ERR_OOM;
        jsonv_arr_set(jsonv_arena, row_arr, (int)col, jsonv_val_str(val_str));
      }
      jsonv_arr_set(jsonv_arena, rows_arr, row_idx++, jsonv_val_arr(row_arr));
    }
  }

  *out_val = jsonv_val_arr(rows_arr);
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t xml_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, XmlConvention conv, Jsonv_Value *out_val) {
  /*#region*/
  if (!out_val) return ERR_INVALID_BOUNDARY;
  XmlParser parser = { .data = data.data, .len = data.length, .pos = 0 };
  
  xml_skip_ws(&parser);
  
  XmlNode *root_node = parse_xml_element(arena, &parser);
  if (!root_node) return ERR_TRANSCODE;

  if (conv == XML_PARKER) {
    Jsonv_Obj *outer = jsonv_obj_new(jsonv_arena, NULL);
    if (!outer) return ERR_OOM;
    char *root_key = allocate_jsonv_string_from_sv(arena, root_node->tag);
    if (!root_key) return ERR_OOM;
    jsonv_obj_set(jsonv_arena, outer, root_key, xml_to_json_parker(arena, jsonv_arena, root_node));
    *out_val = jsonv_val_obj(outer);
  } else if (conv == XML_BADGERFISH) {
    Jsonv_Obj *outer = jsonv_obj_new(jsonv_arena, NULL);
    if (!outer) return ERR_OOM;
    char *root_key = allocate_jsonv_string_from_sv(arena, root_node->tag);
    if (!root_key) return ERR_OOM;
    jsonv_obj_set(jsonv_arena, outer, root_key, xml_to_json_badgerfish(arena, jsonv_arena, root_node));
    *out_val = jsonv_val_obj(outer);
  } else if (conv == XML_JSONML) {
    *out_val = xml_to_json_jsonml(arena, jsonv_arena, root_node);
  } else {
    return ERR_TRANSCODE;
  }

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t form_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, Jsonv_Value *out_val) {
  /*#region*/
  if (!out_val) return ERR_INVALID_BOUNDARY;
  Jsonv_Obj *obj = jsonv_obj_new(jsonv_arena, NULL);
  if (!obj) return ERR_OOM;

  size_t pos = 0;
  while (pos < data.length) {
    size_t pair_end = pos;
    while (pair_end < data.length && data.data[pair_end] != '&') {
      pair_end++;
    }
    size_t eq = pos;
    while (eq < pair_end && data.data[eq] != '=') {
      eq++;
    }
    StringView key_sv = { data.data + pos, eq - pos };
    StringView val_sv = { NULL, 0 };
    if (eq < pair_end) {
      val_sv.data = data.data + eq + 1;
      val_sv.length = pair_end - eq - 1;
    }
    size_t key_d_len = 0, val_d_len = 0;
    char *key_str = percent_decode(arena, key_sv.data, key_sv.length, &key_d_len);
    char *val_str = percent_decode(arena, val_sv.data, val_sv.length, &val_d_len);
    if (!key_str || !val_str) return ERR_OOM;
    jsonv_obj_set(jsonv_arena, obj, key_str, jsonv_val_str(val_str));
    pos = pair_end + 1;
  }
  *out_val = jsonv_val_obj(obj);
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t binary_decode(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, BinaryEncoding enc, Jsonv_Value *out_val) {
  /*#region*/
  (void)jsonv_arena;
  if (!out_val) return ERR_INVALID_BOUNDARY;
  size_t out_len = 0;
  char *decoded_str = NULL;
  if (enc == BINARY_BASE64) {
    decoded_str = base64_decode(arena, data.data, data.length, &out_len);
  } else if (enc == BINARY_HEX) {
    decoded_str = hex_decode(arena, data.data, data.length, &out_len);
  }
  if (!decoded_str) return ERR_TRANSCODE;
  *out_val = jsonv_val_str(decoded_str);
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t yaml_to_json(Arena *arena, Jsonv_Arena *jsonv_arena, StringView data, Jsonv_Value *out_val) {
  /*#region*/
  if (!out_val) return ERR_INVALID_BOUNDARY;
  
  char *yaml_cstr = na_alloc(arena, data.length + 1);
  if (!yaml_cstr) return ERR_OOM;
  memcpy(yaml_cstr, data.data, data.length);
  yaml_cstr[data.length] = '\0';

  Jsonv_Context *ctx = jsonv_ctx_new(jsonv_arena, NULL, NULL);
  if (!ctx) return ERR_OOM;
  
  if (jsonv_ctx_parse_yaml_data(ctx, (const unsigned char *)yaml_cstr)) {
    jsonv_ctx_get_value(ctx, out_val);
    return ERR_SUCCESS;
  }
  return ERR_TRANSCODE;
  /*#endregion*/
}

int32_t json_to_csv(Arena *arena, Jsonv_Value val, CsvOptions opts, StringView *out_sv) {
  /*#region*/
  if (val.tag != JSONV_VAL_ARRAY) return ERR_TRANSCODE;
  Jsonv_Arr *rows_arr = val.as.p;
  int rows_len = jsonv_arr_length(rows_arr);
  char delim = opts.delimiter == '\0' ? ',' : opts.delimiter;

  size_t buf_cap = 1024;
  char *buf = na_alloc(arena, buf_cap);
  if (!buf) return ERR_OOM;
  size_t buf_len = 0;

  if (opts.header) {
    if (rows_len > 0) {
      Jsonv_Value first_row;
      if (jsonv_arr_get(rows_arr, 0, &first_row) && first_row.tag == JSONV_VAL_OBJ) {
        Jsonv_Obj *obj = first_row.as.p;
        int keys_count = jsonv_obj_length(obj);
        for (int col = 0; col < keys_count; col++) {
          if (col > 0) {
            if (buf_len + 1 >= buf_cap) {
              buf_cap += 256;
              char *new_buf = na_alloc(arena, buf_cap);
              if (!new_buf) return ERR_OOM;
              memcpy(new_buf, buf, buf_len);
              buf = new_buf;
            }
            buf[buf_len++] = delim;
          }
          const char *key = jsonv_obj_key_at(obj, col);
          int32_t status = write_csv_cell(arena, &buf, &buf_len, &buf_cap, delim, key, strlen(key));
          if (status != ERR_SUCCESS) return status;
        }
        if (buf_len + 1 >= buf_cap) {
          buf_cap += 256;
          char *new_buf = na_alloc(arena, buf_cap);
          if (!new_buf) return ERR_OOM;
          memcpy(new_buf, buf, buf_len);
          buf = new_buf;
        }
        buf[buf_len++] = '\n';
      }
    }
  }

  for (int r = 0; r < rows_len; r++) {
    Jsonv_Value row_val;
    if (!jsonv_arr_get(rows_arr, r, &row_val)) continue;
    
    if (row_val.tag == JSONV_VAL_OBJ) {
      Jsonv_Obj *obj = row_val.as.p;
      Jsonv_Value first_row;
      if (jsonv_arr_get(rows_arr, 0, &first_row) && first_row.tag == JSONV_VAL_OBJ) {
        Jsonv_Obj *schema_obj = first_row.as.p;
        int keys_count = jsonv_obj_length(schema_obj);
        for (int col = 0; col < keys_count; col++) {
          if (col > 0) {
            if (buf_len + 1 >= buf_cap) {
              buf_cap += 256;
              char *new_buf = na_alloc(arena, buf_cap);
              if (!new_buf) return ERR_OOM;
              memcpy(new_buf, buf, buf_len);
              buf = new_buf;
            }
            buf[buf_len++] = delim;
          }
          const char *key = jsonv_obj_key_at(schema_obj, col);
          Jsonv_Value cell_val;
          const char *val_str_data = "";
          size_t val_len = 0;
          char tmp[64];
          if (jsonv_obj_get(obj, key, &cell_val)) {
            if (cell_val.tag == JSONV_VAL_STRING) {
              val_str_data = (const char *)cell_val.as.p;
              val_len = jsonv_val_str_len(cell_val);
            } else if (cell_val.tag == JSONV_VAL_INT) {
              val_len = snprintf(tmp, sizeof(tmp), "%lld", (long long)cell_val.as.i);
              val_str_data = tmp;
            } else if (cell_val.tag == JSONV_VAL_DOUBLE) {
              val_len = snprintf(tmp, sizeof(tmp), "%g", cell_val.as.d);
              val_str_data = tmp;
            } else if (cell_val.tag == JSONV_VAL_BOOLEAN) {
              val_str_data = cell_val.as.boolean ? "true" : "false";
              val_len = strlen(val_str_data);
            } else if (cell_val.tag == JSONV_VAL_NULL) {
              val_str_data = "null";
              val_len = 4;
            }
          }
          int32_t status = write_csv_cell(arena, &buf, &buf_len, &buf_cap, delim, val_str_data, val_len);
          if (status != ERR_SUCCESS) return status;
        }
      }
    } else if (row_val.tag == JSONV_VAL_ARRAY) {
      Jsonv_Arr *arr = row_val.as.p;
      int arr_len = jsonv_arr_length(arr);
      for (int col = 0; col < arr_len; col++) {
        if (col > 0) {
          if (buf_len + 1 >= buf_cap) {
            buf_cap += 256;
            char *new_buf = na_alloc(arena, buf_cap);
            if (!new_buf) return ERR_OOM;
            memcpy(new_buf, buf, buf_len);
            buf = new_buf;
          }
          buf[buf_len++] = delim;
        }
        Jsonv_Value cell_val;
        const char *val_str_data = "";
        size_t val_len = 0;
        char tmp[64];
        if (jsonv_arr_get(arr, col, &cell_val)) {
          if (cell_val.tag == JSONV_VAL_STRING) {
            val_str_data = (const char *)cell_val.as.p;
            val_len = jsonv_val_str_len(cell_val);
          } else if (cell_val.tag == JSONV_VAL_INT) {
            val_len = snprintf(tmp, sizeof(tmp), "%lld", (long long)cell_val.as.i);
            val_str_data = tmp;
          } else if (cell_val.tag == JSONV_VAL_DOUBLE) {
            val_len = snprintf(tmp, sizeof(tmp), "%g", cell_val.as.d);
            val_str_data = tmp;
          } else if (cell_val.tag == JSONV_VAL_BOOLEAN) {
            val_str_data = cell_val.as.boolean ? "true" : "false";
            val_len = strlen(val_str_data);
          } else if (cell_val.tag == JSONV_VAL_NULL) {
            val_str_data = "null";
            val_len = 4;
          }
        }
        int32_t status = write_csv_cell(arena, &buf, &buf_len, &buf_cap, delim, val_str_data, val_len);
        if (status != ERR_SUCCESS) return status;
      }
    }
    if (buf_len + 1 >= buf_cap) {
      buf_cap += 256;
      char *new_buf = na_alloc(arena, buf_cap);
      if (!new_buf) return ERR_OOM;
      memcpy(new_buf, buf, buf_len);
      buf = new_buf;
    }
    buf[buf_len++] = '\n';
  }
  out_sv->data = buf;
  out_sv->length = buf_len;
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t json_to_xml(Arena *arena, Jsonv_Value val, XmlConvention conv, StringView *out_sv) {
  /*#region*/
  if (!out_sv) return ERR_INVALID_BOUNDARY;
  size_t buf_cap = 1024;
  char *buf = na_alloc(arena, buf_cap);
  if (!buf) return ERR_OOM;
  size_t buf_len = 0;

  int32_t status = ERR_SUCCESS;
  if (conv == XML_PARKER || conv == XML_BADGERFISH) {
    if (val.tag != JSONV_VAL_OBJ) return ERR_TRANSCODE;
    Jsonv_Obj *obj = val.as.p;
    if (jsonv_obj_length(obj) != 1) return ERR_TRANSCODE;
    const char *root_tag = jsonv_obj_key_at(obj, 0);
    Jsonv_Value root_val = jsonv_obj_val_at(obj, 0);
    status = json_to_xml_helper(arena, &buf, &buf_len, &buf_cap, root_val, (StringView){ root_tag, strlen(root_tag) }, conv);
  } else if (conv == XML_JSONML) {
    status = json_to_xml_helper(arena, &buf, &buf_len, &buf_cap, val, (StringView){ NULL, 0 }, conv);
  } else {
    return ERR_TRANSCODE;
  }

  if (status != ERR_SUCCESS) return status;
  out_sv->data = buf;
  out_sv->length = buf_len;
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t json_to_form(Arena *arena, Jsonv_Value val, StringView *out_sv) {
  /*#region*/
  if (val.tag != JSONV_VAL_OBJ) return ERR_TRANSCODE;
  Jsonv_Obj *obj = val.as.p;
  int len = jsonv_obj_length(obj);
  size_t buf_cap = 256;
  char *buf = na_alloc(arena, buf_cap);
  if (!buf) return ERR_OOM;
  size_t buf_len = 0;

  for (int i = 0; i < len; i++) {
    if (i > 0) {
      if (buf_len + 1 >= buf_cap) {
        buf_cap += 256;
        char *new_buf = na_alloc(arena, buf_cap);
        if (!new_buf) return ERR_OOM;
        memcpy(new_buf, buf, buf_len);
        buf = new_buf;
      }
      buf[buf_len++] = '&';
    }
    const char *key = jsonv_obj_key_at(obj, i);
    size_t key_len = strlen(key);
    Jsonv_Value item_val = jsonv_obj_val_at(obj, i);
    
    size_t key_e_len = 0;
    char *key_e = percent_encode_str(arena, key, key_len, &key_e_len);
    if (!key_e) return ERR_OOM;

    const char *val_str_data = "";
    size_t val_len = 0;
    char tmp[64];
    if (item_val.tag == JSONV_VAL_STRING) {
      val_str_data = (const char *)item_val.as.p;
      val_len = jsonv_val_str_len(item_val);
    } else if (item_val.tag == JSONV_VAL_INT) {
      val_len = snprintf(tmp, sizeof(tmp), "%lld", (long long)item_val.as.i);
      val_str_data = tmp;
    } else if (item_val.tag == JSONV_VAL_DOUBLE) {
      val_len = snprintf(tmp, sizeof(tmp), "%g", item_val.as.d);
      val_str_data = tmp;
    } else if (item_val.tag == JSONV_VAL_BOOLEAN) {
      val_str_data = item_val.as.boolean ? "true" : "false";
      val_len = strlen(val_str_data);
    } else if (item_val.tag == JSONV_VAL_NULL) {
      val_str_data = "null";
      val_len = 4;
    }

    size_t val_e_len = 0;
    char *val_e = percent_encode_str(arena, val_str_data, val_len, &val_e_len);
    if (!val_e) return ERR_OOM;

    size_t needed = key_e_len + 1 + val_e_len;
    if (buf_len + needed >= buf_cap) {
      buf_cap += needed + 256;
      char *new_buf = na_alloc(arena, buf_cap);
      if (!new_buf) return ERR_OOM;
      memcpy(new_buf, buf, buf_len);
      buf = new_buf;
    }
    memcpy(buf + buf_len, key_e, key_e_len);
    buf_len += key_e_len;
    buf[buf_len++] = '=';
    memcpy(buf + buf_len, val_e, val_e_len);
    buf_len += val_e_len;
  }
  out_sv->data = buf;
  out_sv->length = buf_len;
  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t binary_encode(Arena *arena, StringView data, BinaryEncoding enc, StringView *out_sv) {
  /*#region*/
  if (!out_sv) return ERR_INVALID_BOUNDARY;
  size_t out_len = 0;
  char *encoded_str = NULL;
  if (enc == BINARY_BASE64) {
    encoded_str = base64_encode(arena, data.data, data.length, &out_len);
  } else if (enc == BINARY_HEX) {
    encoded_str = hex_encode(arena, data.data, data.length, &out_len);
  }
  if (!encoded_str) return ERR_OOM;
  out_sv->data = encoded_str;
  out_sv->length = out_len;
  return ERR_SUCCESS;
  /*#endregion*/
}
