# Nestor Integration Runtime: Technical Roadmap

This roadmap outlines the technical phases and milestones for Nestor's development, transitioning from an in-memory execution pipeline to a compiled virtual machine and a stateful integration server.

---

## Phase 1: Stateless CLI Integration Runtime (In-Memory Engine)

Phase 1 focuses on building the core C execution engine, memory systems, expression parsers, and task/concurrency schedulers.

### Stages 1 to 8: Foundational AST Engine
*   **Stage 1: Memory & State Structures**: Core chained arena allocators (`arena.c`), string view helpers (`stringview.c`), and bit-packed execution state stacks (`bitstack.c`).
*   **Stage 2: Schema Parser**: AST construction bridge using the `jsonv` validation parser library.
*   **Stage 3: DAG Compiler**: Topological sorting of execution steps using Kahn's algorithm and dependency cycle checks.
*   **Stage 4: Expression Evaluator**: Direct thread-local integration with the C-JSONata engine to parse dynamic variables.
*   **Stage 5: HTTP Native Step Runner**: Non-blocking network clients using `curl_multi` loop integrations.
*   **Stage 6: Control Flow Nodes**: Logical routing gateway nodes (`if`, `switch`, and `loop`).
*   **Stage 7: Concurrency Engine**: Fork/Join parallel execution boundaries.
*   **Stage 8: Observability & Redaction**: Secure Aho-Corasick log scanner redacting runtime secrets (`***`) on the fly.

### Additional Phase 1 Extensions
*   **Stage 8.1: Async Subprocess Plugin Timeouts**: Plugin execution boundary timeouts.
*   **Stage 8.2: Concurrency Throttling**: Semaphore-based runtime throttling of parallel tasks.
*   **Stage 8.3: Step Retry Policies**: Exponential/linear retry and backoff error recovery.
*   **Stage 8.4: Loop Compaction**: Iterative sub-arena compaction, cleaning scratch frames.
*   **Stage 8.5: Dedicated Log Redirection**: Separated stderr stream logging for plugins.
*   **Stage 8.6: Interactive Unix IPC Socket API**: Direct subprocess queries for host context variables.
*   **Stage 8.7: SQLite-Backed Cache**: Fast job caching with HTTP revalidations, TTL, and LRU size pruning.
*   **Stage 8.8: Edge-Based Conditions**: Job-property edge triggers (`onSuccess`, `onFailure`, `onSkip`, `onCompletion`).
*   **Stage 8.9: Native Transform Job**: In-process JSONata mappings without process forks.
*   **Stage 8.10: Plugin Sandboxing**: Forked subprocess isolation boundaries for untrusted code.

### Phase 1 compilation & VM Targets
*   **Stage 9: Provider SDK & C SPI**: Standard API boundaries to load dynamic libraries (`.so`/`.dylib`) as providers.
*   **Stage 9.5: Multi-File Workspace Loader**: Recursive folder scanning, compiling workspaces dynamically.
*   **Stage 10: Multi-Format Transcoder & JSON-IR**:
    *   **Stage 10.1: CSV/TSV Transcoder**: Zero-copy RFC 4180 CSV parser producing `Jsonv_Value` arrays. Configurable delimiter, header mode, and quote relaxation.
    *   **Stage 10.2: XML Transcoder**: SAX-like single-pass XML scanner with three conversion conventions (Parker, BadgerFish, JsonML).
    *   **Stage 10.3: URL-Encoded Form Transcoder**: `application/x-www-form-urlencoded` parser and serializer for OAuth2 and webhook payloads.
    *   **Stage 10.4: Binary Encoding**: Base64 and hex encode/decode functions operating on arena `StringView` buffers.
    *   **Stage 10.5: YAML Transcoder**: Parse YAML data payloads (distinct from workflow loader) into `Jsonv_Value`.
    *   **Stage 10.6: Output Formatters**: Reverse serializers (`$csvFormat`, `$xmlFormat`, `$formEncode`, `$binaryEncode`) converting `Jsonv_Value` back to target format strings.
    *   **Stage 10.7: JSONata Function Registration**: Register all transcoder functions (`$csvParse`, `$xmlParse`, `$formParse`, `$binaryDecode`, `$yamlParse`, and their output counterparts) in the evaluation environment.
*   **Stage 10.8: Nested Sub-Workflows**: Execution frames allowing recursive nested scenarios.
*   **Stage 11: Static Schema Verification & Context Inspection**:
    *   **Stage 11.1: Context Inspection (`plan --show-context`)**: Dry-run workflows with mock payloads, printing the transcoded JSON context tree.
    *   **Stage 11.2: Sample Anchoring (`schema_sample`)**: Compile-time validation of downstream expressions against transcoded sample files.
    *   **Stage 11.3: Contract Type Validation**: Pre-flight type checks and dry-runs (`plan`).
*   **Stage 11.5: Bytecode Compiler**: Serializing AST scenarios to binary bytecode (`.nbc`).
*   **Stage 11.7: Nestor VM (NVM)**: Mapped zero-parsing VM interpreter (`apply`) running instructions.

---

## Phase 1.5: Declarative C Pipelines & Terraform Paradigms

This phase introduces stateful capabilities and advanced scoping rules to optimize memory and lifecycle tracking.

*   **Stage 14: Job/Step Variables**: Lexical scoping rules, name validation, and Public vs. Private scopes.
*   **Stage 14.5: Outcome Projections**: Step-level projection filters, immediately releasing parent payload memory.
*   **Stage 15: Session Pools**: Global HTTP and database connection pooling.
*   **Stage 15.5: State Backend (.tfstate)**: Serialization and file locking for execution suspension.
*   **Stage 16: Read/Write Split**: Parallel caching for Data Sources, serialization for Resources.
*   **Stage 16.5: Declarative YAML Providers**: Direct YAML contract configurations mapping functions to operations.

---

## Phase 2: Gateway Daemon & Business APIs (Stateful Server)

Phase 2 transitions the Integration Runtime to Server Mode, wrapping workflows in API gateways.

*   **Stage 12: Gateway Daemon**: Embedding `libmicrohttpd` to expose endpoints, deploying schemas, and running REST-to-workflow bindings.
*   **Stage 13: Webhook Correlation**: Correlation routing, waking up suspended processes on webhook triggers.
