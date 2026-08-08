# Nestor Orchestration Engine: Technical Implementation Specification

---

## 1. System Architecture Overview

Nestor is implemented in pure C for minimal overhead, zero-copy parsing, and absolute memory efficiency. By default, scenarios are compiled to bytecode and executed on the Nestor Virtual Machine (NVM).

```mermaid
graph TB
    subgraph Input & Validation
        stdin[YAML/JSON input] -->|CLI Mode| Parser[jsonv Parser]
        API[API HTTP/gRPC] -->|Server Mode| Parser
        Schema[workflow_schema.json] -->|Validation| Parser
    end

    subgraph Default VM Compiler & Interpreter
        Parser -->|AST Output| CompilerDAG[DAG Compiler]
        CompilerDAG -->|Check Cycles / Toposort| BytecodeComp[Bytecode Compiler]
        BytecodeComp -->|.nbc Bytecode| NVM[Nestor VM Interpreter]
        NVM -->|HTTP Requests| LibCurl[libcurl Bridge]
        NVM -->|Expressions / JSONata| JSONata[jsonata Bridge]
        NVM -->|DB Persistence / Caching| SQLite[(sqlite3)]
    end

    subgraph Legacy execution path (Deprecated)
        CompilerDAG -.->|Legacy AST Path| LegacyDAG[Legacy DAG Runner]
    end

    subgraph Observability
        NVM -->|Logs Stream| AhoCorasick[Aho-Corasick Redactor]
        AhoCorasick -->|Safe Output| SafeLogs[Log Sink]
    end
```

---

## 2. Memory Management & Zero-Copy Architecture

### 2.1 Chained Arena Allocator
All dynamic memory allocations in Nestor must use a custom **Chained Arena Allocator**. The use of standard library heap allocators (`malloc`, `free`, `realloc`, `calloc`) is strictly forbidden during execution.

```c
typedef struct ArenaChunk ArenaChunk;
struct ArenaChunk {
    uint8_t* memory;
    size_t capacity;
    size_t offset;
    ArenaChunk* next;
};

typedef struct {
    ArenaChunk* first;
    ArenaChunk* current;
    size_t default_chunk_size;
} Arena;
```

#### Arena Rules:
*   **Explicit Context Passing**: Every function requiring allocation must accept a pointer to the active `Arena` (e.g. `void* my_func(Arena* arena, ...)`).
*   **Graceful OOM Handling**: If a chunk allocation fails, the allocator returns `NULL`. The code must check for this, clean up resources, and gracefully propagate `ERR_OOM`.
*   **Single-Pass Deallocation**: Memory reclamation is done in bulk at iteration frame boundaries using `arena_reset`.

### 2.2 Zero-Copy String Views
String values (keys, URLs, expressions, bodies) are never duplicated during parsing or symbol extraction. Instead, they reference slices of the original input buffer using a length-delimited `StringView`:

```c
typedef struct {
    const char* data;  // Pointer directly to original input buffer
    size_t length;     // Character count (not null-terminated)
} StringView;
```

---

## 3. Dynamic Integration Bridges

### 3.1 `jsonv` (YAML/JSON Parser & Validator)
*   **Bridge Strategy**: Configure `jsonv` with custom allocators pointing to our `Arena` via callback wrappers to prevent standard heap allocation.
*   **Loader Verification**: The loader parses YAML files into a JSONv structure to inspect root keys. If a root key `"provider"` (of type string) is detected, the file is classified as a provider wrapper; otherwise, it is compiled as a workflow scenario.
*   **Dynamic Schema Compilation**: If `docs/workflow_schema.bin` does not exist or is modified, the parser automatically compiles `docs/workflow_schema.json` to binary bytecode.
*   **Parent Path Fallback**: If the schema files are not found in the current working directory, the parser falls back to checking `../docs/` and `../../docs/` directories to support execution from workspace subfolders.

### 3.2 `jsonata` (Query & Transformation Engine)
*   **Bridge Strategy**: Nestor instantiates a thread-local JSONata evaluation environment. It evaluates expressions enclosed in `${{ ... }}` by compiling the paths into a JSONata tree and evaluating them against the context scope.

### 3.3 `libcurl` (HTTP Transport)
*   **Bridge Strategy**: Utilizes curl's non-blocking `curl_multi` interface. In streaming step mode (`stream: true`), curl write callbacks dump received bytes to a file on disk in fixed chunks to avoid in-memory DOM allocations.

### 3.4 `sqlite3` (Event-Sourced Persistence & Cache Subsystem)
*   **Pruning & WAL**: Configured to run in WAL mode (`PRAGMA journal_mode=WAL;`).
*   **Eviction**: Eviction routines (TTL and LRU size pruning) are run inside single SQLite write transactions on every cache insertion.
*   **Bypass Overrides**: Caching is bypassed globally if `NESTOR_NO_CACHE=true` (CLI flag `--no-cache`), or individually at the job level if `cache: false` or if the job-level cache TTL expires.

### 3.5 Dynamic Link Plugins (Plugin SDK & Sandboxing)
*   **Sandboxed Subprocess**: If `sandboxed: true` is set, Nestor forks a helper subprocess runner (`nestor-plugin-runner`) to isolate dynamic library loading, catching segmentation faults and leaks before they reach the main host thread.
*   **Trusted Dynamic Loading**: If sandboxing is disabled, the plugin loads via `dlopen()`. Memory is allocated from the host-supplied `Arena*` using a double-pointer registration struct.

---

## 4. Intrusive Data Structures

To eliminate pointer indirection, Nestor embeds topological links directly within the job structures:

```c
typedef enum {
    NODE_TASK,
    NODE_IF,
    NODE_SWITCH,
    NODE_FORK,
    NODE_JOIN,
    NODE_LOOP,
    NODE_WAIT_SIGNAL,
    NODE_WAIT_TIMER,
    NODE_TRANSFORM,
    NODE_EXPORT
} NodeType;

struct JobNode {
    NodeType type;
    StringView id;
    StringView name;

    JobNode* next_sorted;  // topologically sorted linked list
    JobNode** depends_on_nodes;  // Resolved dependency pointers
    StringView* depends_on_ids;  // Raw dependency IDs
    uint8_t* depends_on_conditions; // Bitmask conditional edges
    size_t dependency_count;

    uint8_t execution_state; // PENDING, RUNNING, SUCCEEDED, FAILED, SKIPPED, SUSPENDED

    bool is_start;
    bool is_end;
    StringView return_expr;

    bool cache_enabled;
    bool has_cache_ttl;
    int32_t cache_ttl;

    union {
        struct {
            struct StepNode* steps_head;
        } task;

        struct {
            StringView expression;
        } transform;

        struct {
            StringView file_path;
            Jsonv_Value data_val;
        } export_node;

        struct {
            StringView condition;
            StringView* then_branch;
            size_t then_count;
            StringView* else_branch;
            size_t else_count;
        } binary_if;

        // Multi-case switch, loop, join, and wait structures...
    } spec;
};
```

---

## 5. Default VM Execution Engine & Interpreter

Nestor's default runtime execution engine is the **Nestor Virtual Machine (NVM)**. It replaces the legacy AST interpretation engine (`runner.c`), executing compiled bytecode sequences with minimal overhead.

### 5.1 Stack-Based VM Model
NVM is a stack-based machine operating on a sequential array of instructions.
*   **Evaluation Stack**: A bounded array of size `VM_STACK_LIMIT` containing `Jsonv_Value` items. Used for passing operands and storing local intermediate results.
*   **Call Stack**: A stack tracking return PCs and isolated frame contexts during sub-workflow calls.
*   **Registers**:
    *   `PC` (Program Counter): Points to the instruction about to be read from the bytecode.
    *   `SP` (Stack Pointer): Tracks the active top of the evaluation stack.

### 5.2 Compiling & Mapped Execution
1.  **AST-to-Bytecode Compilation**: The bytecode compiler (`bytecode_compiler.c`) performs a topological sort on workflow jobs and compiles variables, loops, control flows, and steps into standard big-endian opcodes.
2.  **Mmap Loading**: The VM maps bytecode files (`.nbc`) into memory using `mmap()` (zero-copy overhead).
3.  **Execute Loop**: Iterates through instructions using a switch-based decoding loop. Each opcode acts directly on the evaluation stack and triggers native curl, JSONata, or provider queries.

### 5.3 Bit-Packed State Stack
For loops and parallel branches, the interpreter tracks nested iterations and states without allocation by compressing scopes and condition bits into a single `uint64_t` stack (`BitStack`).

### 5.4 Scoped Variable Resolution
*   **VM Scope Frames**: When entering a block, the VM pushes a new frame context. Variables are resolved dynamically by looking up key mappings on the evaluation stack and parent frame context.
*   **Visibility**: Private variables exist purely in memory during execution. Public variables are serialized to the state context under the job outputs object.

### 5.5 Legacy AST Runner (runner.c)
The previous interpreter (`runner.c`) directly navigated the AST nodes to resolve dependencies and evaluate steps. This engine is kept for regression checks but is deactivated in default runtime environments.

---

## 6. Security & Secret Redaction (Aho-Corasick)
Any standard output or logging string generated during execution passes through an inline Aho-Corasick matching trie. Sensitive keys/values loaded in the context are compiled into the state machine, and matches are redacted in-place (`***`) in zero-allocation buffers.

---

## 7. Error Handling & Propagation
All routines return signed `int32_t` status codes:
*   `ERR_SUCCESS` (`0`): Execution completed successfully.
*   `ERR_OOM` (`-1`): Arena allocation failed or stack overflow occurred.
*   `ERR_CYCLIC_DEP` (`-2`): Cyclic dependency in variable/graph compilation.
*   `ERR_MISSING_VAR` (`-3`): Expression evaluated to null for a required key.
*   `ERR_CLI_UNSUPPORTED_BLOCKING_NODE` (`-4`): Suspended state gate hit in stateless CLI.
*   `ERR_LOOP_MAX_ITERATIONS` (`-5`): Loop exceeded maximum iteration threshold.
*   `ERR_HTTP_TRANSPORT` (`-6`): Request timed out or failed to connect.
*   `ERR_INVALID_BOUNDARY` (`-7`): Start/end graph boundary violations.
*   `ERR_LOCKED` (`-8`): State backend lock file active.
*   `ERR_TRANSCODE` (`-9`): Data format transcoding failed (malformed CSV, invalid XML, unsupported encoding).

---

## 8. Multi-Format Transcoder Module

### 8.1 Architecture: JSON as Unified IR

Nestor uses `Jsonv_Value` (the arena-allocated JSON DOM) as its **Unified Intermediate Representation**. Rather than building separate C type hierarchies for each data format, all non-JSON inputs are transcoded into `Jsonv_Value` structures at the expression evaluation boundary, and formatted back to the target encoding only at output boundaries.

```
 Raw String (CSV/XML/Binary)
       │
       ▼
 ┌─────────────────────────┐
 │  Transcoder (Input)     │  $csvParse(), $xmlParse(), ...
 │  Arena-allocated parse  │
 └──────────┬──────────────┘
            │ Jsonv_Value
            ▼
 ┌─────────────────────────┐
 │  Core Engine            │  JSONata evaluator, DAG runner, NVM
 │  (operates on JSON IR)  │
 └──────────┬──────────────┘
            │ Jsonv_Value
            ▼
 ┌─────────────────────────┐
 │  Formatter (Output)     │  $csvFormat(), $xmlFormat(), ...
 │  Serialize to string    │
 └─────────────────────────┘
```

**Rationale**: This approach avoids VM instruction bloat (no new opcodes), preserves JSONata compatibility (one query language for all formats), and keeps the zero-copy arena memory model intact.

### 8.2 Input Transcoders

All transcoders accept an `Arena*` parameter and produce `Jsonv_Value` results without heap allocation.

#### `csv_to_json(Arena* arena, StringView data, CsvOptions opts)` → `Jsonv_Value`
*   Scans line-by-line using pointer arithmetic over the original buffer (zero-copy for unquoted fields).
*   If `opts.header == true`: extracts the first line as `StringView` keys, produces `JSONV_VAL_ARRAY` of `JSONV_VAL_OBJ`.
*   If `opts.header == false`: produces `JSONV_VAL_ARRAY` of `JSONV_VAL_ARRAY`.
*   Handles RFC 4180 quoted fields (double-quote escaping) by copying only quoted cells into arena memory.
*   Supports configurable delimiter (`','`, `';'`, `'\t'`) and relaxed parsing mode (tolerating ragged rows).

#### `xml_to_json(Arena* arena, StringView data, XmlConvention conv)` → `Jsonv_Value`
*   Implements a SAX-like single-pass scanner tracking element nesting depth via the existing `BitStack`.
*   Produces `Jsonv_Value` trees according to the selected convention:
    *   `XML_PARKER`: Direct tag-to-key mapping. Single children become values; repeated siblings fold into arrays.
    *   `XML_BADGERFISH`: Attributes stored with `@` prefix, text content under `$` key.
    *   `XML_JSONML`: Preserves document order using nested arrays (`["tag", {"@attr": "val"}, ...children]`).
*   Does not support XML namespaces or DTD validation (out of scope for an integration runtime).

#### `form_to_json(Arena* arena, StringView data)` → `Jsonv_Value`
*   Parses `application/x-www-form-urlencoded` key-value pairs into a flat `JSONV_VAL_OBJ`.
*   Handles percent-decoding (`%20` → space) and `+` substitution in-place on arena-copied segments.

#### `binary_decode(Arena* arena, StringView data, BinaryEncoding enc)` → `Jsonv_Value`
*   Decodes `base64` or `hex` encoded strings into raw byte `StringView` values stored in the arena.
*   Returns a `JSONV_VAL_STRING` containing the decoded bytes.

### 8.3 Output Formatters

Formatters serialize `Jsonv_Value` back into target format strings for export jobs, HTTP request bodies, or plugin outputs.

*   **`json_to_csv(Arena* arena, Jsonv_Value val, CsvOptions opts)`** → `StringView`: Extracts unique keys from the first object in the array to write headers, then iterates row objects to write delimited values.
*   **`json_to_xml(Arena* arena, Jsonv_Value val, XmlConvention conv)`** → `StringView`: Reconstructs XML element trees from JSON objects, respecting the convention used during parsing.
*   **`json_to_form(Arena* arena, Jsonv_Value val)`** → `StringView`: Serializes flat objects to `key=value&key2=value2` with percent-encoding.
*   **`binary_encode(Arena* arena, StringView data, BinaryEncoding enc)`** → `StringView`: Encodes raw bytes to base64 or hex strings.

### 8.4 JSONata Function Registration

Transcoders are exposed to the user as custom JSONata functions registered in the evaluation environment (`evaluator.c`):

| JSONata Function | C Implementation | Description |
| :--- | :--- | :--- |
| `$csvParse(str, opts?)` | `csv_to_json()` | Parse CSV/TSV string to JSON array |
| `$csvFormat(val, opts?)` | `json_to_csv()` | Serialize JSON array to CSV string |
| `$xmlParse(str, convention?)` | `xml_to_json()` | Parse XML string to JSON tree |
| `$xmlFormat(val, convention?)` | `json_to_xml()` | Serialize JSON tree to XML string |
| `$formParse(str)` | `form_to_json()` | Parse URL-encoded string to JSON object |
| `$formEncode(val)` | `json_to_form()` | Serialize JSON object to URL-encoded string |
| `$binaryDecode(str, encoding)` | `binary_decode()` | Decode base64/hex string to raw bytes |
| `$binaryEncode(str, encoding)` | `binary_encode()` | Encode raw bytes to base64/hex string |
| `$yamlParse(str)` | `yaml_to_json()` | Parse YAML string to JSON value |

Functions are registered via the existing `jsonata_register_function()` C bridge, receiving the active `Arena*` as their allocation context. Return values are `Jsonv_Value` pointers that integrate seamlessly with the rest of the expression evaluation pipeline.

