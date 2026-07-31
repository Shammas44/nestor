#include "cache.h"
#include "sha256.h"
#include "error_codes.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

void *na_alloc(Arena *arena, size_t size);

static sqlite3 *cache_db = NULL;
static int cache_max_entries = 1000;

static char *arena_strdup(Arena *arena, const char *str) {
  /*#region*/
  if (!str) return NULL;
  size_t len = strlen(str);
  char *new_str = na_alloc(arena, len + 1);
  if (new_str) {
    memcpy(new_str, str, len);
    new_str[len] = '\0';
  }
  return new_str;
  /*#endregion*/
}

int32_t cache_init(const char *db_path, int max_entries) {
  /*#region*/
  if (cache_db) {
    return ERR_SUCCESS;
  }

  int rc = sqlite3_open(db_path, &cache_db);
  if (rc != SQLITE_OK) {
    if (cache_db) {
      sqlite3_close(cache_db);
      cache_db = NULL;
    }
    return ERR_HTTP_TRANSPORT; // Or dynamic plugin error
  }

  sqlite3_busy_timeout(cache_db, 5000);

  char *err_msg = NULL;
  rc = sqlite3_exec(cache_db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    if (err_msg) sqlite3_free(err_msg);
    sqlite3_close(cache_db);
    cache_db = NULL;
    return ERR_HTTP_TRANSPORT;
  }

  const char *schema_sql =
    "CREATE TABLE IF NOT EXISTS nestor_cache (\n"
    "    cache_key TEXT PRIMARY KEY,\n"
    "    job_id TEXT NOT NULL,\n"
    "    job_type TEXT NOT NULL,\n"
    "    status_code INTEGER,\n"
    "    headers TEXT,\n"
    "    output_payload TEXT,\n"
    "    etag TEXT,\n"
    "    last_modified TEXT,\n"
    "    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
    "    last_accessed_at DATETIME DEFAULT CURRENT_TIMESTAMP,\n"
    "    expires_at DATETIME\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS idx_cache_expiry ON nestor_cache (expires_at);\n"
    "CREATE INDEX IF NOT EXISTS idx_cache_accessed ON nestor_cache (last_accessed_at);\n";

  rc = sqlite3_exec(cache_db, schema_sql, NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    if (err_msg) sqlite3_free(err_msg);
    sqlite3_close(cache_db);
    cache_db = NULL;
    return ERR_HTTP_TRANSPORT;
  }

  cache_max_entries = max_entries;
  return ERR_SUCCESS;
  /*#endregion*/
}

void cache_close(void) {
  /*#region*/
  if (cache_db) {
    sqlite3_close(cache_db);
    cache_db = NULL;
  }
  /*#endregion*/
}

void cache_generate_key(const char *job_type, const char *spec_json, const char *inputs_json, const char *env_json, char *out_hex) {
  /*#region*/
  SHA256_CTX ctx;
  sha256_init(&ctx);

  if (job_type) {
    sha256_update(&ctx, (const uint8_t *)job_type, strlen(job_type));
  }
  if (spec_json) {
    sha256_update(&ctx, (const uint8_t *)spec_json, strlen(spec_json));
  }
  if (inputs_json) {
    sha256_update(&ctx, (const uint8_t *)inputs_json, strlen(inputs_json));
  }
  if (env_json) {
    sha256_update(&ctx, (const uint8_t *)env_json, strlen(env_json));
  }

  uint8_t hash[32];
  sha256_final(&ctx, hash);

  for (int i = 0; i < 32; i++) {
    snprintf(out_hex + (i * 2), 3, "%02x", hash[i]);
  }
  out_hex[64] = '\0';
  /*#endregion*/
}

int32_t cache_lookup(Arena *arena, const char *key, long *status_code_out, char **headers_json_out, char **output_payload_out, char **etag_out, char **last_modified_out) {
  /*#region*/
  if (!cache_db || !key) {
    return ERR_MISSING_VAR;
  }

  const char *query =
    "SELECT status_code, headers, output_payload, etag, last_modified FROM nestor_cache\n"
    "WHERE cache_key = ? AND (expires_at IS NULL OR expires_at >= datetime('now'));";

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(cache_db, query, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    return ERR_HTTP_TRANSPORT;
  }

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    if (status_code_out) {
      *status_code_out = (long)sqlite3_column_int64(stmt, 0);
    }
    if (headers_json_out) {
      const char *val = (const char *)sqlite3_column_text(stmt, 1);
      *headers_json_out = val ? arena_strdup(arena, val) : NULL;
    }
    if (output_payload_out) {
      const char *val = (const char *)sqlite3_column_text(stmt, 2);
      *output_payload_out = val ? arena_strdup(arena, val) : NULL;
    }
    if (etag_out) {
      const char *val = (const char *)sqlite3_column_text(stmt, 3);
      *etag_out = val ? arena_strdup(arena, val) : NULL;
    }
    if (last_modified_out) {
      const char *val = (const char *)sqlite3_column_text(stmt, 4);
      *last_modified_out = val ? arena_strdup(arena, val) : NULL;
    }

    sqlite3_finalize(stmt);

    // Update last_accessed_at
    const char *update_sql = "UPDATE nestor_cache SET last_accessed_at = datetime('now') WHERE cache_key = ?;";
    sqlite3_stmt *update_stmt = NULL;
    if (sqlite3_prepare_v2(cache_db, update_sql, -1, &update_stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(update_stmt, 1, key, -1, SQLITE_STATIC);
      sqlite3_step(update_stmt);
      sqlite3_finalize(update_stmt);
    }

    return ERR_SUCCESS;
  }

  sqlite3_finalize(stmt);
  return ERR_MISSING_VAR; // Cache miss
  /*#endregion*/
}

int32_t cache_store(const char *key, const char *job_id, const char *job_type, long status_code, const char *headers_json, const char *output_payload, const char *etag, const char *last_modified, int ttl_seconds) {
  /*#region*/
  if (!cache_db || !key || !job_id || !job_type) {
    return ERR_MISSING_VAR;
  }

  int rc = sqlite3_exec(cache_db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    return ERR_HTTP_TRANSPORT;
  }

  // 1. Prune expired entries
  rc = sqlite3_exec(cache_db, "DELETE FROM nestor_cache WHERE expires_at < datetime('now');", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(cache_db, "ROLLBACK;", NULL, NULL, NULL);
    return ERR_HTTP_TRANSPORT;
  }

  // 2. Insert or replace new entry
  const char *insert_sql =
    "INSERT OR REPLACE INTO nestor_cache (cache_key, job_id, job_type, status_code, headers, output_payload, etag, last_modified, expires_at, last_accessed_at)\n"
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, datetime('now', '+' || ? || ' seconds'), datetime('now'));";
  const char *insert_sql_no_ttl =
    "INSERT OR REPLACE INTO nestor_cache (cache_key, job_id, job_type, status_code, headers, output_payload, etag, last_modified, expires_at, last_accessed_at)\n"
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, NULL, datetime('now'));";

  sqlite3_stmt *stmt = NULL;
  if (ttl_seconds > 0) {
    rc = sqlite3_prepare_v2(cache_db, insert_sql, -1, &stmt, NULL);
  } else {
    rc = sqlite3_prepare_v2(cache_db, insert_sql_no_ttl, -1, &stmt, NULL);
  }

  if (rc != SQLITE_OK) {
    sqlite3_exec(cache_db, "ROLLBACK;", NULL, NULL, NULL);
    return ERR_HTTP_TRANSPORT;
  }

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, job_id, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, job_type, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, status_code);
  if (headers_json) {
    sqlite3_bind_text(stmt, 5, headers_json, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  if (output_payload) {
    sqlite3_bind_text(stmt, 6, output_payload, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  if (etag) {
    sqlite3_bind_text(stmt, 7, etag, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 7);
  }
  if (last_modified) {
    sqlite3_bind_text(stmt, 8, last_modified, -1, SQLITE_STATIC);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  if (ttl_seconds > 0) {
    sqlite3_bind_int(stmt, 9, ttl_seconds);
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_exec(cache_db, "ROLLBACK;", NULL, NULL, NULL);
    return ERR_HTTP_TRANSPORT;
  }

  // 3. LRU Eviction
  sqlite3_stmt *count_stmt = NULL;
  rc = sqlite3_prepare_v2(cache_db, "SELECT COUNT(*) FROM nestor_cache;", -1, &count_stmt, NULL);
  int count = 0;
  if (rc == SQLITE_OK) {
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
      count = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);
  }

  if (count > cache_max_entries) {
    int limit_val = count - cache_max_entries;
    const char *evict_sql =
      "DELETE FROM nestor_cache WHERE cache_key IN (\n"
      "    SELECT cache_key FROM nestor_cache\n"
      "    ORDER BY last_accessed_at ASC\n"
      "    LIMIT ?\n"
      ");";
    sqlite3_stmt *evict_stmt = NULL;
    rc = sqlite3_prepare_v2(cache_db, evict_sql, -1, &evict_stmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int(evict_stmt, 1, limit_val);
      sqlite3_step(evict_stmt);
      sqlite3_finalize(evict_stmt);
    }
  }

  sqlite3_exec(cache_db, "COMMIT;", NULL, NULL, NULL);
  return ERR_SUCCESS;
  /*#endregion*/
}
