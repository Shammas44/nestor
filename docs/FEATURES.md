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

---

## 7. Multi-Format Data Transcoding

Nestor uses JSON as its **Unified Intermediate Representation (IR)** internally. All non-JSON data formats are converted to and from JSON at the execution boundary, allowing the user to write a single query language (JSONata) regardless of the source format.

### 7.1 Design Principle: Explicit User-Controlled Parsing
Data received from external systems (HTTP responses, file reads, plugin outputs) is kept as a **raw string** by default. The user explicitly invokes transcoding functions inside JSONata expressions, giving them full control over when and how conversion happens:

```yaml
steps:
  - id: process_report
    type: transform
    spec:
      expression: "$csvParse(jobs.fetch.steps.get_data.body, {'header': true}).rows[status = 'active']"
```

This avoids implicit "magic" conversions and lets the user choose the mapping convention that best fits their data.

### 7.2 Supported Formats

| Priority | Format | JSONata Function | Description |
| :--- | :--- | :--- | :--- |
| Tier 0 | **JSON** | *(native)* | Core IR. All internal processing operates on JSON values. |
| Tier 1 | **CSV / TSV** | `$csvParse(str, opts)` | Batch data, database dumps, billing reports, spreadsheet imports. |
| Tier 1 | **XML** | `$xmlParse(str, opts)` | Legacy enterprise backends, SOAP services, RSS feeds, financial gateways. |
| Tier 1 | **URL-Encoded** | `$formParse(str)` / `$formEncode(obj)` | OAuth2 token exchanges, HTML form submissions, webhook payloads. |
| Tier 1 | **Binary** | `$binaryEncode(str, enc)` / `$binaryDecode(str, enc)` | File uploads/downloads (PDF, images), S3 object transfers. |
| Tier 2 | **YAML** | `$yamlParse(str)` | Configuration file processing, Kubernetes manifest generation. |
| Future | **Protobuf / gRPC** | *(planned)* | Low-latency internal microservice mesh transcoding. |

### 7.3 Conversion Conventions

#### CSV Mapping
*   **With headers** (`header: true`, default): Returns an **Array of Objects** where keys are column names.
    *   Input: `name,age\nAlice,30` → Output: `[{"name": "Alice", "age": "30"}]`
*   **Without headers** (`header: false`): Returns an **Array of Arrays**.
    *   Input: `Alice,30` → Output: `[["Alice", "30"]]`
*   Configurable delimiter (`","`, `";"`, `"\t"`) and quote-relaxation options.

#### XML Mapping
Three conventions are supported to handle the full spectrum of XML complexity:
*   **Parker** (default, simplest): Tags map directly to keys. Attributes are discarded. Duplicate siblings fold into arrays.
    *   Input: `<book><title>Dune</title></book>` → Output: `{"book": {"title": "Dune"}}`
*   **BadgerFish** (lossless primitives): Preserves attributes via `@` prefix and text content via `$` key.
    *   Input: `<book id="1">Dune</book>` → Output: `{"book": {"@id": "1", "$": "Dune"}}`
*   **JsonML** (lossless document order): Preserves exact element ordering and mixed text content using nested arrays.
    *   Input: `<p>Hello <b>bold</b> text</p>` → Output: `["p", "Hello ", ["b", "bold"], " text"]`

### 7.4 Schema Observability
To help users understand how non-JSON data maps to the JSON IR, Nestor provides two complementary tools:

*   **Context Inspection** (`plan --show-context`): Dry-run a workflow with mock payloads and print the transcoded JSON context tree, letting the user see the exact shape of the data before writing expressions.
*   **Sample Anchoring** (`schema_sample`): Reference a local sample file in a step definition. The compiler transcodes the sample at compile time and validates that all downstream expressions reference valid paths, catching errors before execution.

