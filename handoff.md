# Handoff: Nestor VM Phase 2.5 Implementation

## Project Context
*   **Repository Root**: `/Users/sebastientraber/Documents/Prog/c/nestor`
*   **Active Worktree**: `/Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv`
*   **Current Branch**: `dev` (fully merged and committed)
*   **Last Commit**: `a9936e036ce8c15e0cd969010217c8253b851766`

---

## 1. Completed Work (Phase 2)
All 64 unit tests and 22 E2E integration tests are now passing successfully under the Nestor VM (`nvm.c`):
*   Implemented thread execution controls for loops, forks, and joins.
*   Enforced dependency gates within `OP_CALL_PROVIDER` for non-join jobs, preventing out-of-order executions.
*   Supported SQLite-backed caching and 304 validation checks.
*   Documented technical specifications in `docs/ISA.md` and `docs/IMPLEMENTATION.md` to reflect the upcoming Phase 2.5 architecture.

---

## 2. Next Milestone: Phase 2.5 (Low-Level VM & Hermetic Plugin VM)
The goal of this phase is to transition NVM into a pure, decoupled execution engine.

### Next Steps:
1.  **Stage 20: Low-Level Bytecode Transition**:
    *   Refactor `bytecode_compiler.c` to emit primitive jump (`OP_JUMP`), conditional branch (`OP_JUMP_IF_FALSE`), thread spawning (`OP_FORK`), and join synchronization (`OP_JOIN`) instructions.
    *   Decouple the interpreter loop in `nvm.c` completely from AST memory structures (`ctx->ast`).
2.  **Stage 21: Cryptographic Binary Signing**:
    *   Implement Ed25519 asymmetric signature generation in the compiler and verification checks at NVM header loading.
3.  **Stage 22: Unified Plugin Architecture**:
    *   Decouple caching, state persistence, Aho-Corasick redaction, and transport layers as hot-swappable plugins conforming to the host VM integration interface.
4.  **Stage 23: Disassembly Tooling (`nestor-dis`)**:
    *   Develop the `bin/nestor-dis` utility to print bytecode binaries in readable assembly formats.

---

## 3. Reference Documentation
*   [docs/ISA.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/ISA.md)
*   [docs/IMPLEMENTATION.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/IMPLEMENTATION.md)
*   [docs/ROADMAP.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/ROADMAP.md)
*   [docs/PROGRESS.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/PROGRESS.md)

---

## 4. Test Commands
Always clean up stale state lock files before running tests:
```bash
OPTION=dev make clean all
rm -f .*tfstate*
MallocNanoZone=0 bin/test_runner_unit --ignore-warnings -j1
MallocNanoZone=0 bin/test_runner_e2e --ignore-warnings -j1
```
