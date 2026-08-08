# Nestor Integration Runtime: Implementation Progress

This document tracks the current completion status of stages outlined in the [ROADMAP.md](ROADMAP.md).

---

## Phase 1: Stateless CLI Integration Runtime (In-Memory Engine)

*   [x] **Stage 1: Core Memory Management & Bit-Packed Utilities**
    *   [x] Chained Arena Allocator (`arena.c`)
    *   [x] Length-delimited String View helpers (`stringview.c`)
    *   [x] Bit-packed execution state stack (`bitstack.c`)
*   [x] **Stage 2: Schema Parser & AST Construction (`jsonv` Bridge)**
*   [x] **Stage 3: DAG Compiler & Kahn's Sorting**
*   [x] **Stage 4: JSONata Integration & Core Expression Evaluator**
*   [x] **Stage 5: HTTP Native Step Runner**
*   [x] **Stage 6: Control Flow Nodes (If, Switch, Loops)**
*   [x] **Stage 7: Concurrency Engine (In-Memory Fork & Join)**
*   [x] **Stage 8: Observability & Secrets Redaction (Aho-Corasick)**
*   [x] **Stage 8.1: Subprocess Plugin Async Timeouts & Execution Bounds**
*   [x] **Stage 8.2: Concurrent Job Semaphores (Concurrency Throttling)**
*   [x] **Stage 8.3: Step/Job Retry Policies (Transient Error Handling)**
*   [x] **Stage 8.4: Loop Iteration Memory Compaction & Sub-Arenas**
*   [x] **Stage 8.5: Dedicated Stderr Stream Logging for Subprocess Plugins**
*   [x] **Stage 8.6: Interactive Unix Socket IPC API for Dynamic Plugin Context Queries**
*   [x] **Stage 8.7: SQLite-Backed Cache & Eviction Strategy**
*   [x] **Stage 8.8: Edge-Based Conditional Execution**
*   [x] **Stage 8.9: Native JSONata Transform Job**
*   [x] **Stage 8.10: Dynamic Plugin SDK & Subprocess Sandboxing**
*   [x] **Stage 9: Provider SDK & Service Provider Interface (SPI)**
*   [x] **Stage 9.5: Multi-File Workspace Loader & Dependency Compiler**
*   [x] **Stage 10: Multi-Format Transcoder & JSON-IR**
    *   [x] Stage 10.1: CSV/TSV Transcoder (`csv_to_json`)
    *   [x] Stage 10.2: XML Transcoder (`xml_to_json`)
    *   [x] Stage 10.3: URL-Encoded Form Transcoder (`form_to_json`)
    *   [x] Stage 10.4: Binary Encoding (`binary_encode` / `binary_decode`)
    *   [x] Stage 10.5: YAML Transcoder (`yaml_to_json`)
    *   [x] Stage 10.6: Output Formatters (`$csvFormat`, `$xmlFormat`, `$formEncode`, `$binaryEncode`)
    *   [x] Stage 10.7: JSONata Function Registration
*   [x] **Stage 10.8: Nested Sub-Workflow Execution Stack**
*   [x] **Stage 11: Static Schema Verification & Context Inspection**
    *   [x] Stage 11.1: Context Inspection (`plan --show-context`)
    *   [x] Stage 11.2: Sample Anchoring (`schema_sample`)
    *   [x] Stage 11.3: Contract Type Validation & CLI Tooling
*   [x] **Stage 11.5: Bytecode Compiler & NBC Exporter**
*   [x] **Stage 11.7: Nestor VM (NVM) Runtime Execution Loop (Default Engine)**


---

## Phase 1.5: Declarative C Pipelines & Terraform Paradigms

*   [x] **Stage 14: Job/Step Variables (Public vs. Private)**
    *   [x] Enforce snake_case/camelCase keys and resolve cycle checks (`ERR_CYCLIC_DEP`)
    *   [x] Support private variable scope (non-serialized) and public outputs (`jobs.<id>.outputs.<name>`)
*   [x] **Stage 14.5: Step-Level Outcome Projection**
    *   [x] Project subset fields immediately and release large response payloads from arena memory
*   [x] **Stage 15: Global Provider Configurations & Session Reuse**
    *   [x] Centralized provider blocks for Postgres/HTTP pooling and secrets injection
*   [x] **Stage 15.5: State Backends & Execution Locking (.nestor.tfstate)**
    *   [x] Serialize state frames and lock engine runs on asynchronous gates
*   [x] **Stage 16: Data Sources vs. Resources Split**
    *   [x] Cache read-only data sources; serialize write resources under strict order
*   [x] **Stage 16.5: Declarative YAML Providers**
    *   [x] Allow no-code YAML provider mappings to wrap native features, SQL, and plugins

---

## Phase 2: Gateway Daemon & Business APIs (Stateful Server)

*   [ ] **Stage 12: Gateway Daemon & HTTP REST Mapping**
*   [ ] **Stage 13: Webhook Routing & Correlation Resuming**
