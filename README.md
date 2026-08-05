# Nestor Workflow Orchestration Engine

Nestor is a high-performance, memory-optimized workflow execution engine written in C. It models integrations as structure-validated execution graphs, optimizing for minimal latency, zero-copy parsing, and deterministic arena memory footprints.

---

## Quick Start Guide

### 1. Prerequisites & Compilation
Nestor compiles into a single executable binary. Ensure you have standard build tools, `libcurl`, `sqlite3`, and `jsonv`/`jsonata` installed.

Build the binary:
```bash
# Clean and compile with dev flags
OPTION=dev make clean all
```

Run tests to verify the setup:
```bash
# Run unit and end-to-end test suites
bin/test_runner_unit
bin/test_runner_e2e
```

### 2. Create your first Workflow Scenario (`hello.yaml`)
Create a workflow scenario file containing task steps:
```yaml
version: "2.0.0"
name: hello_world
on: { manual: {} }

jobs:
  say_hello:
    type: task
    steps:
      - id: get_ip
        http:
          method: GET
          url: "https://api.ipify.org?format=json"
```

### 3. Execute the Workflow
To execute the workflow scenario, pipe the file content into the engine binary:
```bash
cat hello.yaml | bin/main
```

To run a workspace directory structure containing subfolders for providers and workflows:
```bash
# 1. Verify the workspace dependencies and contracts
bin/main plan ./my_workspace

# 2. Compile the workspace into bytecode (.nbc)
bin/main compile ./my_workspace -o output.nbc

# 3. Execute the bytecode using the Nestor VM (NVM)
bin/main apply output.nbc
```

---

## Command-Line Subcommands

*   `plan <workspace_directory>`: Parses and validates the workspace integrity, checking for dependency cycles and type contract matches.
*   `compile <workspace_directory> -o <output.nbc>`: Serializes the validated workspace graph and string constant pool to a binary bytecode file.
*   `apply <file.nbc>`: Runs the compiled bytecode file directly on the NVM stack-based runtime interpreter with sub-microsecond cold starts.

---

## Detailed Documentation

Complete design, architecture, and specifications are located in the [docs/](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs) folder:

*   **[OVERVIEW.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/OVERVIEW.md)**: Rationale, design philosophy, and core objectives.
*   **[ARCHITECTURE.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/ARCHITECTURE.md)**: System design diagram, components, and zero-copy/arena decisions.
*   **[FEATURES.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/FEATURES.md)**: Summary of job typologies, scoping, big data streaming, and caches.
*   **[IMPLEMENTATION.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/IMPLEMENTATION.md)**: In-depth technical specifications of types, memory arenas, bridges, and scopes.
*   **[ISA.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/ISA.md)**: Detailed NVM Instruction Set Architecture and binary bytecode formats.
*   **[USAGE.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/USAGE.md)**: Formal scenario configuration guide, steps parameters, variables, and examples.
*   **[ROADMAP.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/ROADMAP.md)**: Sequential slices and technical phases of the project.
*   **[PROGRESS.md](file:///Users/sebastientraber/Documents/Prog/c/nestor/.worktrees/json-validation-jsonv/docs/PROGRESS.md)**: Current completion checklist for all developmental stages.
