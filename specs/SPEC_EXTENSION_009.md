# Nestor Orchestration Engine: Phase 1 Extension & Architecture Specification
**Document ID:** Nestor-RFC-009  
**Status:** APPROVED  
**Author:** Core Architecture Group  
**Subject:** Technical Specification for Conditional Execution, Native transform Job, Plugin SDK, and SQLite Cache

---

## 1. Overview & Goals

This specification details the technical design for the Phase 1 extensions of Nestor. Nestor is a high-performance, memory-optimized workflow orchestration engine written in C. It enforces strict deterministic memory management via a chained arena allocator, length-delimited string views for zero-copy parsing, and bit-packed state stacks.

### Goals
*   **Edge-Based Conditional Execution:** Enable dynamic DAG execution routing based on the terminal status of upstream jobs (`onSuccess`, `onFailure`, `onCompletion`, `onSkip`).
*   **Native `transform` Job Type:** Introduce a zero-copy, in-process JSONata data transformation node to perform JSON manipulation in-memory.
*   **Plugin SDK with Sandboxing:** Establish a stable C ABI for loading trusted dynamic libraries (`.so`/`.dylib`) in-process, alongside a subprocess sandboxing flag (`sandboxed: true`) to execute untrusted plugins in isolated process spaces.
*   **SQLite-Backed Cache:** Standardize a local caching layer to store job outcomes, complying with HTTP caching headers, and employing an active Time-To-Live (TTL) and Least Recently Used (LRU) eviction strategy.
*   **Configuration Precedence:** Define a command-line interface with strict configuration precedence rules.

### Non-Goals
*   Linking third-party database clients or message queues into the core Nestor binary.
*   Supporting distributed cache backends (e.g., Redis).

---

## 2. Technical Specification

### 2.1 Conditional execution

Nestor evaluates dependency criteria on the graph edges rather than trigger rules on the nodes. A job specifies its dependency list, and each item in that list targets one or more of the following terminal states: `onSuccess`, `onFailure`, `onCompletion`, or `onSkip`.

#### Semantics
*   A downstream job is evaluated when all upstream jobs have entered a terminal state (`STATE_SUCCEEDED`, `STATE_FAILED`, or `STATE_SKIPPED`).
*   For each upstream job, the actual terminal state is checked against the configured edge conditions.
*   For standard (non-Join) jobs, all incoming edge conditions must be satisfied. If any condition is unsatisfied, the downstream job is marked as `STATE_SKIPPED` and skipped states propagate downstream.
*   For Join nodes, the execution state is determined by applying the join strategy (`all`, `any`, `n_required`) over the set of satisfied incoming edges.

#### JSON Schema
```json
{
  "$defs": {
    "dependency": {
      "type": "object",
      "properties": {
        "job": { "type": "string" },
        "conditions": {
          "type": "array",
          "items": {
            "type": "string",
            "enum": ["onSuccess", "onFailure", "onCompletion", "onSkip"]
          },
          "minItems": 1
        }
      },
      "required": ["job"]
    }
  }
}
```

#### Memory Representation
To prevent dynamic allocations during execution, dependency conditions are stored as bitwise masks in the `JobNode` structure:
```c
#define DEP_COND_SUCCESS    (1 << 0)
#define DEP_COND_FAILURE    (1 << 1)
#define DEP_COND_SKIP       (1 << 2)
#define DEP_COND_COMPLETION (DEP_COND_SUCCESS | DEP_COND_FAILURE | DEP_COND_SKIP)
```

---

### 2.2 Native `transform` Job Type

The native `transform` job performs zero-copy, in-process JSON manipulation using Nestor's integrated JSONata C engine. It operates directly on Nestor's memory structures (`Jsonv_Value`), eliminating the CPU and memory serialization overhead of calling an external process.

#### Configuration
```json
{
  "type": "transform",
  "spec": {
    "expression": "jobs.fetch_data.steps.get_api.output.body.sensors[status='active']"
  }
}
```

---

### 2.3 Plugin SDK & Subprocess Sandboxing

Nestor supports compiled dynamic libraries (`.so`/`.dylib`) using a stable ABI. For security isolation, the engine supports a `sandboxed` boolean flag.

```
                    +-----------------------------+
                    |      Job Execution          |
                    +-----------------------------+
                                   |
                         Is sandboxed == true?
                                  / \
                                 /   \
                               /       \
                             Yes        No
                             /            \
              +--------------------+     +-----------------------+
              | Subprocess Wrapper |     | dlopen() In-Process   |
              | (Process Boundary) |     | (Shared Library Hook) |
              +--------------------+     +-----------------------+
```

*   **`sandboxed: false` (Trusted Dynamic Link Libraries):**
    The engine loads the plugin using `dlopen` and calls the execution function in-process. All memory allocations must be requested from the host-provided `Arena` structure, ensuring thread safety and preventing memory leaks.
*   **`sandboxed: true` (Untrusted Plugins):**
    The engine forks a helper subprocess wrapper (`nestor-plugin-runner`) which loads the library, runs the plugin, writes outputs to stdout, and exits. This isolates the main host from segmentation faults or out-of-memory errors in the plugin.

#### Plugin C SDK Interface (`nestor_plugin.h`)
```c
#ifndef NESTOR_PLUGIN_H
#define NESTOR_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#define NESTOR_API_VERSION_MAJOR 1
#define NESTOR_API_VERSION_MINOR 0

#define NESTOR_LOG_DEBUG 0
#define NESTOR_LOG_INFO  1
#define NESTOR_LOG_WARN  2
#define NESTOR_LOG_ERROR 3

typedef struct {
  void* (*alloc)(void *arena_ptr, size_t size);
  void  (*log)(int level, const char *message);
  const char* (*get_variable)(void *ctx_ptr, const char *json_path);
  int32_t (*set_output)(void *ctx_ptr, const char *key, const char *json_val);
} NestorHostAPI;

typedef struct {
  const char *name;
  const char *version;
  const char *author;
  uint32_t api_major;
  uint32_t api_minor;
} NestorPluginInfo;

typedef struct {
  int32_t (*init)(const NestorHostAPI *host);
  int32_t (*execute)(void *arena_ptr, void *ctx_ptr, const char *args_json);
  void    (*shutdown)(void);
} NestorPluginAPI;

#ifdef _WIN32
  #define NESTOR_EXPORT __declspec(dllexport)
#else
  #define NESTOR_EXPORT __attribute__((visibility("default")))
#endif

NESTOR_EXPORT const NestorPluginInfo* nestor_plugin_query(void);
NESTOR_EXPORT int32_t nestor_plugin_register(const NestorHostAPI *host, NestorPluginAPI *out_api);

#endif // NESTOR_PLUGIN_H
```

---

### 2.4 SQLite Cache & Eviction Strategy

Any job output can be cached in a local SQLite database. HTTP jobs automatically respect `Cache-Control` (`max-age`, `no-cache`, `no-store`), `Expires`, and conditional validation headers (`ETag`, `Last-Modified`).

#### Schema
```sql
CREATE TABLE IF NOT EXISTS nestor_cache (
    cache_key TEXT PRIMARY KEY,
    job_id TEXT NOT NULL,
    job_type TEXT NOT NULL,
    status_code INTEGER,
    headers TEXT,            -- Serialized JSON map for HTTP headers
    output_payload TEXT,     -- Serialized JSON map of job outputs
    etag TEXT,
    last_modified TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    last_accessed_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    expires_at DATETIME
);

CREATE INDEX IF NOT EXISTS idx_cache_expiry ON nestor_cache (expires_at);
CREATE INDEX IF NOT EXISTS idx_cache_accessed ON nestor_cache (last_accessed_at);
```

#### Deterministic Cache Key Generation
A cache key is a SHA-256 hash computed over:
$$\text{Key} = \text{SHA256}(\text{JobType} \parallel \text{JobSpecJSON} \parallel \text{ResolvedInputsJSON} \parallel \text{EnvironmentVariables})$$

#### Eviction Strategy
1.  **TTL-Based Expiration:**
    *   During cache lookup, any row where `expires_at` is less than the current time is ignored.
    *   A pruning query is executed on every write transaction to remove expired rows:
        ```sql
        DELETE FROM nestor_cache WHERE expires_at < datetime('now');
        ```
2.  **LRU Size-Capping Eviction:**
    *   On cache hits, the `last_accessed_at` timestamp is updated:
        ```sql
        UPDATE nestor_cache SET last_accessed_at = datetime('now') WHERE cache_key = ?;
        ```
    *   If a write operation causes the table size to exceed a configured limit (`max_cache_entries`), Nestor triggers an LRU eviction in the same transaction:
        ```sql
        DELETE FROM nestor_cache WHERE cache_key IN (
            SELECT cache_key FROM nestor_cache 
            ORDER BY last_accessed_at ASC 
            LIMIT (SELECT COUNT(*) + 1 - ? FROM nestor_cache)
        );
        ```

---

## 2.5 Command-Line Flags & Precedence

#### Supported Flags
*   `--config <path>`: Global configuration file.
*   `--file <path>`: Input workflow definition file.
*   `--var <key=val>`: Set or override workflow variables.
*   `--secret <key=val>`: Inject secrets.
*   `--no-cache`: Force execution bypassing cache.
*   `--dry-run`: Validate schema and build execution plan without executing tasks.
*   `--validate`: Validate schema and exit.
*   `--verbose` / `--debug` / `--trace`: Adjust logging levels.
*   `--parallelism <n>`: Limit maximum concurrency.

#### Precedence Hierarchy (Highest to Lowest)
1.  **Explicit CLI Flags**
2.  **Environment Variables** (prefixed with `NESTOR_`)
3.  **Workflow-Specific Configurations** (defined in workflow definition file)
4.  **Global/User Configuration File**
5.  **Hardcoded Engine Defaults**
