# Nestor Orchestration Engine: Architecture

Nestor is structured as a modular, stateless workflow pipeline engine in C. It models execution as a Directed Acyclic Graph (DAG) compiled from declarative configurations and run using an interpretation engine or a custom stack-based Virtual Machine.

---

## 1. System Components

```mermaid
graph TB
    subgraph Input & Parsing
        YAML[Workspace Directories] -->|Workspace Loader| Loader[loader.c]
        Loader -->|Check Cycles / Toposort| Compiler[compiler.c]
        Parser[parser.c] -->|Validate Schema| Jsonv[json-validation-jsonv]
    end

    subgraph Runtime Execution (AST Mode)
        Compiler -->|Execution AST| Runner[runner.c]
        Runner -->|Variables / JSONata| Evaluator[evaluator.c]
        Runner -->|HTTP Transport| Transport[transport.c]
        Runner -->|Caching Engine| Cache[cache.c]
        Runner -->|Log Stream| Redactor[redactor.c]
    end

    subgraph Bytecode Execution (VM Mode)
        Compiler -->|Bytecode Compiler| BytecodeComp[bytecode_compiler.c]
        BytecodeComp -->|NBC Bytecode| NVM[nvm.c VM Interpreter]
    end
```

### 1.1 Loader (`loader.c`)
Loads the target workspace directory recursively, parsing all `.yaml`/`.yml` files. It distinguishes between dynamic workflows and declarative provider definitions by inspecting root-level properties, constructs the global workspace graph, and performs static cross-file link validation.

### 1.2 Schema Parser & Validator (`parser.c`)
Integrates the `jsonv` validation parser to enforce structural constraints on incoming workflows. It compiles the external JSON schema (`docs/workflow_schema.json`) to a compiled binary format (`docs/workflow_schema.bin`) for high-speed validations, avoiding schema recompilation on every run.

### 1.3 DAG Compiler (`compiler.c`)
Compiles the parsed workflow tree into an executable Directed Acyclic Graph. It checks that variable references contain no loops, verifies start and end boundaries, topologically sorts jobs using Kahn's algorithm, and validates join dominance to ensure parallel forks converge correctly before workflow exits.

### 1.4 Runtime Execution Engine (`runner.c` / `transport.c` / `cache.c`)
Schedules and advances jobs.
*   **Lexical Scoping**: Pushes and pops environment/job/step scopes dynamically as execution traverses blocks.
*   **HTTP Multiplexing**: Utilizes non-blocking `curl_multi` pools for concurrent network queries.
*   **Caching**: Operates an inline SQLite cache that respects cache-control headers, performing conditional revalidations (`ETag`/`Last-Modified`) and LRU size pruning.

### 1.5 Nestor Virtual Machine (`nvm.c` / `bytecode_compiler.c`)
For low-overhead serverless execution, the compiler maps the topological execution sequence to standard opcodes (`OP_PUSH_CONST`, `OP_CALL_PROVIDER`, `OP_JUMP`, etc.) and serializes it to a single bytecode binary (`.nbc`). The VM interpreter maps the `.nbc` directly via `mmap` and runs instructions inside a secure, bound-checked evaluation stack.

---

## 2. Core Architectural Decisions

### 2.1 The Chained Arena Allocator (Zero-Memory Leak)
*   **Why**: Standard memory management (`malloc` / `free`) is prone to fragmentation and memory leakages under continuous web request loops.
*   **How**: Nestor allocates memory blocks inside a single chained struct pool. Dynamic memory queries grow the active arena chunk. At the end of a request iteration, the entire chain is reset at once, reclaiming all memory in a single instruction.

### 2.2 Zero-Copy String Views (Ultra-Low Footprint)
*   **Why**: Parsing JSON/YAML string objects, environment keys, and request parameters usually duplicates strings onto the heap, creating high allocation overhead.
*   **How**: Nestor represents strings as length-delimited pointers (`StringView`) targeting slices of the original input buffer. They are not null-terminated, which avoids writing changes into read-only mappings.

### 2.3 State Serialization (.tfstate)
*   **Why**: Ephemeral command-line execution cannot handle long-term workflow suspensions (like waiting for a timer or webhook signal) without tying up worker threads.
*   **How**: When hitting a `wait_signal` or timer gate, the runner serializes the active variable stack frames, scheduler queues, and bitstack states to a `.tfstate` file, acquiring a lock file (`.tfstate.lock`) to prevent concurrent race conditions. The workflow exits and frees the host thread, resuming from the serialized state when triggered by external events.
