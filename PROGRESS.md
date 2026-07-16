# nestor Orchestration Engine - Implementation Progress

## Phase 1: In-Memory Engine (Stateless CLI)

- [x] **Stage 1: Core Memory Management & Bit-Packed Utilities**
  - [x] Chained Arena Allocator (`arena.h`, `arena.c`)
  - [x] Length-delimited String View helpers (`stringview.h`, `stringview.c`)
  - [x] Bit-packed execution state stack (`bitstack.h`, `bitstack.c`)
  - [x] Stage 1 unit tests (`tests/test_stage1.c`)
- [x] **Stage 2: Schema Parser & AST Construction (`jsonv` Bridge)**
- [x] **Stage 3: DAG Compiler & Kahn's Sorting**
- [x] **Stage 4: JSONata Integration & Core Expression Evaluator**
- [x] **Stage 5: HTTP Native Step Runner**
- [x] **Stage 6: Control Flow Nodes (If, Switch, Loops)**
- [x] **Stage 7: Concurrency Engine (In-Memory Fork & Join)**
- [x] **Stage 8: Observability & Secrets Redaction (Aho-Corasick)**
- [x] **Stage 8.1: Subprocess Plugin Async Timeouts & Execution Bounds**
- [x] **Stage 8.2: Concurrent Job Semaphores (Concurrency Throttling)**
- [x] **Stage 8.3: Step/Job Retry Policies (Transient Error Handling)**
- [x] **Stage 8.4: Loop Iteration Memory Compaction & Sub-Arenas**
- [x] **Stage 8.5: Dedicated Stderr Stream Logging for Subprocess Plugins**
- [x] **Stage 8.6: Interactive Unix Socket IPC API for Dynamic Plugin Context Queries**
- [x] **Architectural Refactorings**:
  - [x] **Candidate 1**: Deepen Subprocess Plugin Executor (`plugin.h`, `plugin.c`)
  - [x] **Candidate 2**: Decouple HTTP execution via Transport Seam (`transport.h`, `transport.c`)
  - [x] **Candidate 3**: Unify Expression Evaluation, Interpolation, and Duration parsing (`evaluator.h`, `evaluator.c`)


## Phase 2: Long-Lived Workflows (Stateful Server)

- [ ] **Stage 9: Event-Sourced Database Persistence**
- [ ] **Stage 10: Asynchronous State Suspension & Correlation Resuming**
- [ ] **Stage 11: Daemon API & Coordination Daemon (`libmicrohttpd`)**
