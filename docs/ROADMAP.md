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
    *   **Stage 10.1: CSV/TSV Transcoder**: Zero-copy RFC 4180 CSV parser producing `Jsonv_Value` arrays.
    *   **Stage 10.2: XML Transcoder**: SAX-like single-pass XML scanner with Parker, BadgerFish, and JsonML conventions.
    *   **Stage 10.3: URL-Encoded Form Transcoder**: `application/x-www-form-urlencoded` parser and serializer.
    *   **Stage 10.4: Binary Encoding**: Base64 and hex encode/decode functions.
    *   **Stage 10.5: YAML Transcoder**: Parse YAML data payloads into `Jsonv_Value`.
    *   **Stage 10.6: Output Formatters**: Reverse serializers (`$csvFormat`, `$xmlFormat`, `$formEncode`, `$binaryEncode`).
    *   **Stage 10.7: JSONata Function Registration**: Register all transcoder functions.
*   **Stage 10.8: Nested Sub-Workflows**: Execution frames allowing recursive nested scenarios.
*   **Stage 11: Static Schema Verification & Context Inspection**:
    *   **Stage 11.1: Context Inspection (`plan --show-context`)**: Dry-run context variable inspection.
    *   **Stage 11.2: Sample Anchoring (`schema_sample`)**: Compile-time schema-sample validations.
    *   **Stage 11.3: Contract Type Validation**: Pre-flight checks and dry-runs (`plan`).
*   **Stage 11.5: Bytecode Compiler**: Serializing AST scenarios to binary bytecode (`.nbc`).
*   **Stage 11.7: Nestor VM (NVM)**: Mapped zero-parsing VM interpreter (`apply`) running instructions.

---

## Phase 1.5: Declarative C Pipelines & Terraform Paradigms

This phase introduces stateful capabilities and advanced scoping rules to optimize memory and lifecycle tracking.

*   **Stage 14: Job/Step Variables**: Lexical scoping rules, name validation, and Public vs. Private scopes.
*   **Stage 14.5: Outcome Projections**: Step-level projection filters, releasing parent payload memory.
*   **Stage 15: Session Pools**: Global HTTP and database connection pooling.
*   **Stage 15.5: State Backend (.tfstate)**: Serialization and file locking for execution suspension.
*   **Stage 16: Read/Write Split**: Parallel caching for Data Sources, serialization for Resources.
*   **Stage 16.5: Declarative YAML Providers**: Direct YAML contract configurations mapping functions to operations.

---

## Phase 2: Full Nestor VM (NVM) Implementation

This phase completed the stack-based VM runtime execution, enabling NVM to fully replace the legacy AST interpreter runner.

*   **Stage 17: VM Opcode Completeness**: Support for control flow routing, binary conditions, switch cases, loops, fork/join barriers, and error fallback handlers directly inside the NVM interpreter tick loop.
*   **Stage 17.5: Direct VM I/O & Plugin Bridges**: Integration of the non-blocking `curl_multi` transport and subprocess plugin runners into NVM opcodes.
*   **Stage 18: Stack-Frame Scoped Variables & Memory Compaction**: Scoped variable frame boundaries and loop frame compaction sub-arenas.
*   **Stage 18.5: SQLite Caching & Locking in NVM**: Inline caching checks, validation headers handling, and runtime execution locking.
*   **Stage 19: NVM State Serialization & Resuming**: Saving and resuming of VM registers, stack, and frame contexts using `.tfstate` files on wait-signal interruptions.

---

## Phase 2.5: Low-Level Bytecode & Hermetic Plugin VM (Next Milestone)

This phase shifts the NVM execution engine to low-level assembly-like opcodes and modularizes VM interfaces for maximum security and portability.

*   **Stage 20: Low-Level Bytecode Transition**: Refactor the bytecode compiler to emit primitive jump (`OP_JUMP`), conditional branch (`OP_JUMP_IF_FALSE`), thread spawning (`OP_FORK`), and join synchronization (`OP_JOIN`) instructions. Decouple the VM interpreter loop completely from C AST nodes.
*   **Stage 21: Cryptographic Binary Signing**: Implement Ed25519 asymmetric signature generation in the compiler and verification checks in the NVM loader header validation stage to prevent untrusted execution.
*   **Stage 22: Unified Plugin Architecture**: Decouple and modularize core systems (HTTP transport, JSONata transforms, SQLite cache, Aho-Corasick redaction, and state persistence) into hot-swappable plugins conforming to the host VM integration interface.
*   **Stage 23: Disassembly Tooling (`nestor-dis`)**: Build the `bin/nestor-dis` utility to print bytecode binaries in readable assembly formats, showing instruction flows, constants, and signature statuses.

---

## Phase 3: Gateway Daemon & Business APIs (Stateful Server)

Phase 3 transitions the Integration Runtime to Server Mode, wrapping workflows in API gateways.

*   **Stage 24: Gateway Daemon**: Embedding `libmicrohttpd` to expose endpoints, deploying schemas, and running REST-to-workflow bindings.
*   **Stage 25: Webhook Correlation**: Correlation routing, waking up suspended processes on webhook triggers.
