# Handoff: Nestor Integration Runtime Future Paradigms (Phase 1.5) Implementation

You are the next agent in the pair-programming loop. Your task is to implement Phase 1.5 of the **Nestor Integration Runtime (IR)** roadmap, incorporating Terraform-like variables, state backends, session reuse, and declarative YAML providers.

---

## 1. Context & Isolation Status
* **Active Directory Worktree**: [/Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv)
* **Active Branch**: `feature/json-validation-jsonv`

---

## 2. Key Specifications & References
Before starting, read the specification files using the `view_file` tool:
1. **New Feature Specification**: [PROPOSED_FEATURES.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/specs/PROPOSED_FEATURES.md) — Detailed description of scoping, variables, projections, provider blocks, locking, and declarative provider mechanics.
2. **Technical Roadmap**: [ROADMAP.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/specs/ROADMAP.md) — Outlines technical stages (Phase 1.5 Stages 14 to 16.5).
3. **VM Security & ISA**: [SPEC_BYTECODE_VM.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/specs/SPEC_BYTECODE_VM.md) and [ISA_SPECIFICATION.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/specs/ISA_SPECIFICATION.md) — General rules on bytecode format and safety limits.

---

## 3. Strict Rules & Constraints
* **English Comments Only**: All comments and documentation must be in English.
* **No Standard Allocation**: `malloc`/`free`/`calloc` are strictly forbidden. Use `na_alloc(Arena*, size_t)` or `mmap`.
* **Zero-Copy Views**: Keep `StringView` bounds pointing directly to mapped buffer files without string copy duplicates.
* **Fold Boundaries**: Wrap all C function declarations inside folding blocks:
  ```c
  void do_stuff(void) {
    /*#region*/
    // code
    /*#endregion*/
  }
  ```

---

## 4. Implementation Steps

Implement Phase 1.5 incrementally, compiling and running tests at each stage:

### Step 1: Stage 14 — Job/Step Variables (Public vs. Private)
* Parse `variables` blocks at the Job/Step levels in `compiler.c` (validate snake_case/camelCase naming rules).
* Implement ordered dependency resolution and cycle check (`ERR_CYCLIC_DEP`) for variable evaluations.
* Intercept step evaluation in `runner.c` to evaluate local variables. Store `private` variables locally in a temporary frame, and publish `public` variables to `jobs.<id>.outputs.<name>`. Bypass DB serialization for private variables to conserve memory.

### Step 2: Stage 14.5 — Step-Level Outcome Projection
* Add support for `outputs:` projection block mapping in step runner.
* Free the raw response buffer from the arena immediately after projecting keys, retaining only the projected subset.

### Step 3: Stage 15 — Global Provider Configurations & Session Reuse
* Add support for `providers:` blocks in workflow headers.
* Implement reusable session handles (TCP/TLS / SQLite connection pools) managed via global provider handles, sharing them across steps.

### Step 4: Stage 15.5 — State Backends & Execution Locking
* Implement state serialization and deserialization functions dumping context frames and active stacks to `.nestor.tfstate`.
* Add file-locking gate controls to prevent concurrent run collisions.

### Step 5: Stage 16 — Data Sources vs. Resources Split
* Classify step queries into `Data` (read-only, cached) and `Resource` (mutative, uncached).
* Enforce parallel execution for data sources and serial execution for resources.

### Step 6: Stage 16.5 — Declarative YAML Providers
* Add dynamically registered providers mapping YAML operation descriptions to HTTP requests, SQL queries, or local plugins in memory.
