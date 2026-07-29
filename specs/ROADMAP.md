# Nestor Integration Runtime (IR): Phased Technical Roadmap

- **Version:** 3.1.0 (NVM & Bytecode Integration)
- **Author:** Core Architecture Group
- **Subject:** Replacing Glue-Code Microservices with Declarative C Pipelines

---

## 1. Introduction & Implementation Strategy

Nestor is transitioned from a generic workflow orchestrator to a high-performance **Integration Runtime (IR)**. It enables developers to replace boilerplate "glue microservices" with declarative workflows executed on top of a low-overhead, memory-optimized C engine.

### Core Strategic Focus
> **"This platform lets developers replace integration microservices with declarative workflows."**

### 1.1 The "Terraform for Backend Integrations" Analogy
Nestor mirrors HashiCorp Terraform's configuration, provider-based resource modeling, plan validation, and parallel execution engine, but executes backend integrations instead of cloud resources.

### 1.2 Implementation Phases

To ensure stability and quick iteration, the project is divided into two distinct phases. Stage 1 sticks strictly to a daemonless CLI execution model.

```mermaid
graph TD
    subgraph Phase 1: Stateless CLI Integration Runtime (Stage 1 Product)
        S1[Stage 1-8.13: Core Engine Foundation] --> S9[Stage 9: Provider SDK & C SPI]
        S9 --> S95[Stage 9.5: Multi-File Workspace Loader]
        S95 --> S10[Stage 10: Canonical Type Casting]
        S10 --> S105[Stage 10.5: Nested Sub-Workflows]
        S105 --> S11[Stage 11: Static Contract Validation & Plan CLI]
        S11 --> S115[Stage 11.5: Bytecode Compiler & NBC Exporter]
        S115 --> S117[Stage 11.7: Nestor VM Execution Loop]
    end

    subgraph Phase 2: Long-Lived & Exposed Business APIs (Future Server Product)
        S117 --> S12[Stage 12: Gateway Daemon & HTTP REST Mapping]
        S12 --> S13[Stage 13: Webhook Routing & Correlation Resuming]
    end

    style S1 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S9 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S95 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S10 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S105 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S11 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S115 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S117 fill:#1a5f7a,stroke:#333,stroke-width:2px,color:#fff
    style S12 fill:#c45a00,stroke:#333,stroke-width:2px,color:#fff
    style S13 fill:#c45a00,stroke:#333,stroke-width:2px,color:#fff
```

---

## 2. Technical Stack & Verification Rules

Every implementation stage includes a formal verification step. Before proceeding, developers must:
1.  **Compile with dev options**: `OPTION=dev make all`
2.  **Memory safety audit**: Run the test suite under Valgrind to ensure zero leaks or out-of-bounds reads.
3.  **Strict memory law check**: Validate that all dynamic allocations use the custom chained `Arena` allocator (no unmanaged `malloc`/`free` in internal code).

---

## 3. Phase 1: Stateless CLI Integration Runtime

Phase 1 focuses entirely on a daemonless developer CLI tool. The engine takes JSON/YAML payloads from standard input, maps variables, evaluates contracts, executes provider steps, and returns outputs on stdout.

---

### Stages 1 to 8.13: Foundational Engine (Completed)
*   **Core Memory & BITStack Helpers**: Fast chained arena allocation and state tracking.
*   **Compiler & Toposort**: Kahn's DAG sorting with cycle and boundary checking.
*   **JSONata Expression Engine**: Inline `${{ ... }}` interpolation.
*   **Http Stream Transport**: Direct-to-disk HTTP file chunking.
*   **SAX Chunk Loop**: Memory-mapped SAX loops (`stream_chunk`) executing chunks in $O(1)$ memory.

---

### Stage 9: Provider SDK & Service Provider Interface (SPI)

#### Objective
Establish a unified interface allowing dynamic libraries (`.so`/`.dylib` files) to act as Nestor Providers. Develop initial C SPI bindings and implement core providers (Filesystem, HTTP, and Postgres).

#### Files & Structures
*   `src/include/provider.h`
*   `src/comm/struct/provider.c`
*   `providers/postgres/` & `providers/filesystem/`

#### Detailed Steps
1.  Define the Provider SPI function signature mapping. A provider exports a dynamic symbol:
    ```c
    int32_t nestor_provider_execute(
      Arena *arena,
      const char *operation,
      Jsonv_Value args,
      Jsonv_Value *output,
      char *err_buf,
      size_t err_len
    );
    ```
2.  Refactor HTTP execution into a built-in provider module.
3.  Implement the Filesystem provider using raw file and pipe descriptor streams.
4.  Implement the PostgreSQL provider as a dynamic plugin (`dlopen` mapping), translating SQL rows into structured nested tables.

#### E2E Verification
Write `tests/test_stage9.c` loading the filesystem provider dynamically. Trigger file copy and write operations via workflow parameters, asserting that execution is completed using strictly arena-allocated buffers.

---

### Stage 9.5: Multi-File Workspace Loader & Dependency Compiler

#### Objective
Enable Nestor to compile integration projects spread across multiple files under a target directory (providers and sub-workflows), resolving cross-references dynamically.

#### Files & Structures
*   `src/include/loader.h`
*   `src/comm/struct/loader.c`

#### Detailed Steps
1.  Implement a workspace crawler loading all files under `./providers/*.yaml` and `./workflows/*.yaml` in memory.
2.  Store workflows and providers in a global workspace symbol map.
3.  Verify that cross-file links (e.g. `call: workflows.crm_onboard`) refer to valid workspace files.
4.  Run topological validation across the global workspace schema, throwing `ERR_CYCLIC_DEP` if recursive loops exist across workflows.

#### E2E Verification
Define a mock workspace directory. Invoke the loader and verify it correctly compiles and links a main workflow with a sub-workflow without crashing or duplication.

---

### Stage 10: Canonical Type Mapping & Transcoding

#### Objective
Define the canonical intermediate formats of the Nestor runtime (Scalar, Object, List, Table, Document, Binary, Stream) and implement zero-copy transcoders to convert provider inputs and outputs.

#### Files & Structures
*   `src/include/types.h`
*   `src/comm/utils/transcoder.c`

#### Detailed Steps
1.  Implement XML-to-Document converter mapping nodes dynamically to `Jsonv_Value`.
2.  Implement CSV/Relational Rowset to Table transcoding.
3.  Map standard Binary buffers into raw base64 or pointer-backed StringViews within the memory arena.
4.  Validate that transcoders do not copy static string keys, referencing the source HTTP/DB raw buffers directly.

#### E2E Verification
Run mock SQL rowset inputs through the transcoder and verify it maps directly to a nested JSON/YAML table format without memory leaks or allocations outside the active `Arena`.

---

### Stage 10.5: Nested Sub-Workflow Execution

#### Objective
Enable the execution engine to orchestrate sub-workflows by invoking nested runtime frames, passing arguments, and returning context variables.

#### Files & Structures
*   `src/comm/struct/runner.c`

#### Detailed Steps
1.  Implement a step runner module for step type `call`.
2.  When a sub-workflow is invoked, push a new `ActiveJob` execution stack frame onto the scheduler.
3.  Isolate the variable frame by mapping passed `args` directly as the new frame's `inputs`.
4.  Upon nested completion, pop the stack frame and map its `return` outputs back to the calling step's outputs.

#### E2E Verification
Run a workflow containing a nested call to a sub-workflow. Verify that variables are isolated and output returns map cleanly.

---

### Stage 11: Static Schema Contract Verification & CLI Tooling

#### Objective
Build static type checking and variable contract verification into the compile loop to catch input/output type mismatches before workflow execution. Expose commands `nestor plan` (for static dry-runs) and `nestor apply` (for executions).

#### Files & Structures
*   `src/comm/struct/compiler.c`
*   `main.c`

#### Detailed Steps
1.  Define contract rules for each provider operation (e.g. `postgres.query` output matches `Table` type).
2.  Inspect variables referenced in expressions and verify their source steps emit the expected type schemas.
3.  Implement command line switches:
    - `plan`: Parses the workspace, performs dependency/contract validation, and outputs a formatted static validation summary on stdout.
    - `apply` / `run`: Invokes the compiler, performs planning, and executes the compiled DAG against the input context.

#### E2E Verification
Run `nestor plan` on a workflow matching a database `Table` output into an operation expecting a single `Scalar` string. Verify it halts with `ERR_INVALID_CONTRACT` and outputs a descriptive error position before executing.

---

### Stage 11.5: Bytecode Compiler & NBC Exporter

#### Objective
Design and implement the compiler pass that serializes a statically validated workspace into a single `.nbc` binary bytecode file.

#### Files & Structures
*   `src/include/bytecode.h`
*   `src/comm/struct/bytecode_compiler.c`

#### Detailed Steps
1.  Expose the `compile` command line switch: `nestor compile <project_dir> -o <output.nbc>`.
2.  Serialize the Header segment containing Magic ("NEST") and table of offsets.
3.  Implement Constant Pool serialization: deduplicate strings and keys, layout them back-to-back, and save offsets.
4.  Compile the workspace execution graph into an instruction segment (mapping actions to Opcodes and operand indexes).

#### E2E Verification
Compile a customer onboarding workspace to `onboard.nbc`. Inspect the binary output and verify the magic number and offsets match the specification.

---

### Stage 11.7: Nestor VM (NVM) Runtime Execution Loop

#### Objective
Implement the stack-based virtual machine execution engine to run compiled `.nbc` bytecode files with memory safety boundaries.

#### Files & Structures
*   `src/include/nvm.h`
*   `src/comm/struct/nvm.c`

#### Detailed Steps
1.  Implement the command line switch `apply <file.nbc>` to run compiled bytecode.
2.  Use the `mmap` system call to map `<file.nbc>` and cast structs directly (zero parsing latency).
3.  Implement the loop decode-execute loop `nvm_execute_loop()`, checking SP pointer limits and PC jump bounds.
4.  Assert that nested call frame stacks handle stack pushes and pops without leaks.

#### E2E Verification
Execute `onboard.nbc` against a mock customer input and verify it completes successfully, returning the expected JSON outputs to stdout with sub-microsecond cold-start initialization.

---

## 3.5 Phase 1.5: Declarative C Pipelines & Terraform Paradigms

This phase incorporates Terraform-like design patterns to optimize scope encapsulation, memory usage, session management, and contract testing.

### Stage 14: Job/Step Variables (Public vs. Private)
* **Objective**: Introduce a variables block at the Job/Step level.
* **Mechanism**: Sequential ordered resolution with compile-time cycle check (`ERR_CYCLIC_DEP`). Private variables (`visibility: private`) simplify expressions locally and bypass db state serialization to conserve memory. Public variables (`visibility: public`) act as job boundaries (`jobs.<id>.outputs.<name>`).
* **Naming**: Enforce snake_case/camelCase variables to avoid subtraction parser conflicts.

### Stage 14.5: Step-Level Outcome Projection
* **Objective**: Enable memory minimization for large response bodies.
* **Mechanism**: Allow steps to declare an `outputs` mapping projection; release the raw response payload from the arena immediately after step execution completes.

### Stage 15: Global Provider Configurations & Session Reuse
* **Objective**: Centralize provider block setups.
* **Mechanism**: Open TCP/TLS session pools and database pools once globally and inject secrets securely at the provider boundary.

### Stage 15.5: State Backends & Execution Locking (.nestor.tfstate)
* **Objective**: Stateful serialization for long-running workflows.
* **Mechanism**: Dump active variables stack frames and scheduler queue states to a `.nestor.tfstate` file, implementing state locks for concurrent resilience.

### Stage 16: Data Sources vs. Resources Split
* **Objective**: Distinguish read-only operations from stateful mutations.
* **Mechanism**: Allow aggressive caching and concurrent runs for Data Sources, while serializing Resource updates.

### Stage 16.5: Declarative YAML Providers
* **Objective**: Support no-code provider creation using YAML contracts mapping operations to native features, SQL, or scripts.

---

## 4. Phase 2: Gateway Daemon & Business APIs (Future Server)

Phase 2 transitions the Integration Runtime to Server Mode, wrapping declarative workflows in HTTP endpoints.

---

### Stage 12: Gateway Daemon & HTTP REST Mapping

#### Objective
Embed `libmicrohttpd` to expose workflows as REST endpoints, translating incoming request bodies and headers to workflow inputs, and returning workflow outcomes as HTTP responses.

#### Detailed Steps
1.  Start HTTP server mapping paths directly to deployed workflows (e.g. `POST /customer/onboard`).
2.  Inject authorization validators and generate dynamic OpenAPI specifications.

---

### Stage 13: Webhook Routing & Correlation Resuming

#### Objective
Handle external asynchronous signals (webhooks) and route correlation payloads back into suspended workflow state records.
