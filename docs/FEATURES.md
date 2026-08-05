# Nestor Orchestration Engine: High-Level Features

Nestor provides a rich suite of capabilities designed for executing highly performant, resilient, and secure data integration pipelines.

---

## 1. Execution & Graph Routing

### 1.1 Complete DAG Job Taxonomy
Nestor supports multiple native job types configured via the `"type"` key:
*   **`task`**: Runs a sequence of steps (HTTP, plugins, or providers) sequentially.
*   **`transform`**: Evaluates zero-copy in-memory JSONata mappings without subprocess boundaries.
*   **`export`**: Evaluates custom expressions and exports the serialized results to files.
*   **`if` / `switch`**: Evaluates conditions to dynamically route execution down binary or multi-case branches.
*   **`fork` / `join`**: Spawns parallel execution threads and synchronizes them using configured strategies (`all`, `any`, `n_required`).
*   **`loop`**: Runs iterative sub-graphs in `while` or `for_each` loop blocks.
*   **`wait_signal` / `wait_timer`**: Creates asynchronous gates to suspend execution and resume later.

### 1.2 Fine-Grained Edge Dependencies
Instead of generic triggers, Nestor implements edge-level conditions. Downstream jobs evaluate the terminal status of their dependencies and run only if the criteria match (`onSuccess`, `onFailure`, `onSkip`, `onCompletion`).

---

## 2. Dynamic Variables & Scoping
*   **Lexical Variable Scoping**: Variable resolution searches progressively up the execution stack: Step -> Job -> Workflow Root.
*   **Public vs. Private Visibilities**:
    *   `private` (Default): Variables are kept local to the executing block and are ignored during database state serialization to minimize overhead.
    *   `public`: Variables are serialized and published under the job outputs block (`jobs.<id>.outputs.<var>`), making them accessible to downstream jobs.
*   **Safety compilation checks**: Nestor sorts variable dependencies and blocks runs if loops (`ERR_CYCLIC_DEP`) or invalid keys are detected.

---

## 3. Streaming Big Data Pipelines
To handle payloads exceeding several gigabytes without memory exhaustion, Nestor operates streaming pipelines with a constant memory footprint ($O(1)$ RAM scaling):
*   **Event-Driven SAX Tokenizer**: Streams file/socket bytes in fixed 64 KB chunks, triggering callback tokens instead of parsing full DOM trees.
*   **Chunked Loop Iterators**: Iterates over datasets chunk-by-chunk using a `stream_chunk` loop, evaluating record blocks sequentially and reclaiming chunk memory at iteration frames.
*   **Pipe-Based Streaming Plugins**: Streams data directly through stdin/stdout process pipes to dynamic plugins.
*   **Data Sources vs. Resources Split**: Automatically identifies mutative resources (which bypass caching and run serially to avoid race conditions) from side-effect-free data sources.

---

## 4. SQLite Caching & Overrides
*   **HTTP Cache Compliance**: Respects `Cache-Control` directives (`max-age`, `no-cache`, `no-store`), `Expires` headers, and executes HTTP conditional revalidation (`ETag` with `If-None-Match`, `Last-Modified` with `If-Modified-Since`).
*   **Eviction Policies**: Reclaims storage via automated TTL validation and LRU size pruning on every database write.
*   **Granular Cache Controls**: Features global CLI bypass switches (`--no-cache`), job-level caching toggles (`cache: false`), and job-specific TTL overrides (`cache: { ttl: "15m" }`).

---

## 5. Security & Isolation
*   **Plugin Sandboxing**: Trusted plugins load inline inside the host process using the C SPI SDK. Untrusted plugins are isolated in a secure subprocess sandbox (`sandboxed: true`) to prevent host crashes.
*   **Aho-Corasick Secret Redaction**: Compiles all workflow secrets into a high-speed matching trie, redacting them (`***`) from standard output and logging streams on-the-fly.

---

## 6. Sub-Workflows & Declarative Providers
*   **Nested Sub-Workflows**: Modulize scenarios by calling external workflows recursively (`uses: workflows.other_flow`), returning sub-workflow outputs directly to the parent scope.
*   **Declarative Providers**: Expose reusable database, REST API, or plugin operations in YAML definitions without writing C code, injecting configurations and secrets at the provider boundary.
