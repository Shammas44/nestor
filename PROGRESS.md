# nestor Orchestration Engine - Implementation Progress

## Phase 1: In-Memory Engine (Stateless CLI)

- [x] **Stage 1: Core Memory Management & Bit-Packed Utilities**
  - [x] Chained Arena Allocator (`arena.h`, `arena.c`)
  - [x] Length-delimited String View helpers (`stringview.h`, `stringview.c`)
  - [x] Bit-packed execution state stack (`bitstack.h`, `bitstack.c`)
  - [x] Stage 1 unit tests (`tests/test_stage1.c`)
- [x] **Stage 2: Schema Parser & AST Construction (`jsonv` Bridge)**
- [x] **Stage 3: DAG Compiler & Kahn's Sorting**
- [ ] **Stage 4: JSONata Integration & Core Expression Evaluator**
- [ ] **Stage 5: HTTP Native Step Runner**
- [ ] **Stage 6: Control Flow Nodes (If, Switch, Loops)**
- [ ] **Stage 7: Concurrency Engine (In-Memory Fork & Join)**
- [ ] **Stage 8: Observability & Secrets Redaction (Aho-Corasick)**

## Phase 2: Long-Lived Workflows (Stateful Server)

- [ ] **Stage 9: Event-Sourced Database Persistence**
- [ ] **Stage 10: Asynchronous State Suspension & Correlation Resuming**
- [ ] **Stage 11: Daemon API & Coordination Daemon (`libmicrohttpd`)**
