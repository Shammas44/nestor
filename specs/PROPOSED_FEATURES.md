# Specification: Nestor Integration Runtime Future Paradigms

This specification defines the proposed architectural features for the Nestor Integration Runtime (IR) based on Terraform design philosophies. These enhancements focus on contract encapsulation, memory optimization, session reuse, and stateful execution.

---

## Developer Personas & The Provider Boundary

To maintain separation of concerns and scaling efficiency, Nestor divides the integration development lifecycle into two distinct personas, separated by a strict YAML contract boundary:

### 1. Provider Developer
* **Role**: Low-level integration engineering. Provider developers write C/C++ plugins (dynamic libraries `.so` / `.dylib` or sandboxed binaries) or Python script plugins (conforming to the `nestor_plugin` Python SDK) that handle authentication, low-level network protocols, connection pools, and database drivers.
* **APIs**: They work with the low-level Provider SPI (`nestor_provider_execute`), `Arena` memory allocators, native system capabilities, or Python's `NestorPlugin` helper API.
* **Deliverable**: Compiles binary plugins, builds Python script scripts, and publishes a **YAML Provider Contract** (schema file) declaring the exposed operations, required inputs, and return output structures.

### 2. Workflow Developer
* **Role**: High-level declarative orchestration. Workflow developers write YAML files to coordinate provider operations, map variables, configure step sequences, and evaluate conditionals.
* **APIs**: They write high-level pipeline declarations and JSONata expressions. They are completely insulated from low-level memory layout, networking sockets, or pointer operations.
* **Deliverable**: Declarative integration manifests (workflows) compiled and run via `nestor plan` / `nestor apply`.

```
  [ Provider Developer ] ──► Compiles Plugin (.so) + Defines Contract (.yaml)
                                                                 │
                                                                 ▼
  [ Workflow Developer ] ◄── Orchestrates steps matching the Contract schemas
```

---


## 1. Ordered Job/Step Variables (Public vs. Private)

To simplify complex JSONata expressions and decouple downstream jobs from upstream step details, Nestor introduces job-level variable blocks with visibility boundaries.

### 1.1 Variable Scoping & Visibility
* **Private Variables (`visibility: private`)**: Local scope within a single JobNode's execution. They are resolved sequentially and used inside step expressions. To minimize memory footprint, private variables are **not** written to the execution state database.
* **Public Variables (`visibility: public` / Outputs)**: Job-level exports exposed to downstream consumer jobs. Accessible via `jobs.<job_id>.outputs.<variable_name>`. This encapsulates the job's steps (black-box module).

### 1.2 Syntax Example
```yaml
jobs:
  fetch_xwing:
    type: task
    variables:
      - name: get_body
        expression: "steps.get_xwing.body"
        visibility: private
      - name: passenger_count
        expression: "$number(get_body.passengers)"
        visibility: public
    steps:
      - id: get_xwing
        http:
          method: GET
          url: "http://starwars.api/xwing"
```

### 1.3 Structural Rules
1. **No Hyphens**: Variable names must use `snake_case` or `camelCase` to prevent subtraction errors (`x - wing`) in JSONata.
2. **Cycle Check**: The compiler validates variable dependencies; cycles trigger `ERR_CYCLIC_DEP`.

---

## 2. Step-Level Outcome Projection (Memory Minimization)

Currently, Nestor retains full step response bodies in memory to serve downstream queries. For large payloads (e.g. 10MB JSON files), this is highly inefficient.

### 2.1 Projected Outputs
Steps can define an `outputs` block. Once the step completes, the raw payload is garbage collected from the memory arena, retaining only the projected values.

### 2.2 Syntax Example
```yaml
      - id: get_payload
        http: ...
        outputs:
          size_mb: "body.size_mb"
          records: "body.data"
```

---

## 3. Workflow-Level Shared Constants

Root-level variables evaluated once at workflow startup and shared across all jobs in the workflow.

```yaml
version: 2.0.0
name: Customer Sync Pipeline
variables:
  - name: api_url
    expression: "env.STAGE == 'prod' ? 'https://api.prod' : 'https://api.stage'"
```

---

## 4. Step-Level `on_error` Fallbacks (Circuit Breakers)

Declarative fallbacks that prevent execution halts when non-critical step nodes fail.

```yaml
      - id: fetch_metadata
        http: ...
        on_error:
          fallback: { "status": "offline", "data": [] }
```

---

## 5. Global Provider Configurations (Session Reuse)

Instead of passing credentials and URLs in every step, Nestor establishes global provider configurations to reuse TCP/TLS sessions and database connection pools.

```yaml
providers:
  postgres:
    connection_string: "${{ secrets.db_url }}"
    max_connections: 5
  stripe:
    api_key: "${{ secrets.stripe_key }}"

jobs:
  sync:
    steps:
      - id: save_user
        provider: postgres.insert
        args:
          table: "users"
          record: "${{ inputs.record }}"
```

---

## 6. State Backends & Execution Locking

For long-running pipelines (containing webhook suspension gates or timers), Nestor supports saving the active execution context to a State Backend.

* **Serialization**: Pushes active stack frames, scheduler queues, and bitstack states to a `.nestor.tfstate` file.
* **Resuming**: When a webhook arrives, Nestor checks for state lock integrity, loads the tfstate, and resumes execution at the suspension boundary.

---

## 7. Data Sources vs. Resources

To optimize execution paths, steps are classified as either:
* **Data Sources (Read-Only)**: Side-effect-free steps (e.g., `http.get`, `postgres.query`). These can be cached in Nestor's SQLite cache and run concurrently without lock hazards.
* **Resources (Mutative)**: Steps that mutate external state (e.g., `http.post`, `postgres.insert`). These bypass caching completely.

---

## 8. Declarative Providers (No-Code YAML Mapping)

Nestor allows Provider Developers to define providers **strictly in YAML/JSON** without writing compiled C code, wrapping not just HTTP/REST web services, but **any available native features, database queries, and plugins** of Nestor.

### 8.1 Mechanics
A declarative provider contract maps exposed operations directly to Nestor's internal capabilities (HTTP requests, database queries, Python scripts, or compiled shared library step execution). The engine parses this mapping and runs the steps internally, performing zero-copy transcoding of inputs and outputs at the boundary.

### 8.2 Syntax Example (`providers/postgres_helper.yaml`)
Wrapping a database configuration and exposing simplified SQL operations:
```yaml
provider: db_helper
description: Declarative Database wrapper
configuration:
  connection_string: "${{ secrets.db_url }}"

operations:
  fetch_active_users:
    # Maps operation to Nestor's native postgres query plugin
    uses: postgres.query
    args:
      sql: "SELECT id, email FROM users WHERE status = 'active' LIMIT $1"
      params:
        - "${{ inputs.limit }}"
    inputs:
      limit: { type: number, required: true }
    outputs:
      users: "body.rows"
```

### 8.3 Benefits
1. **Multi-Protocol Wrapping**: Declarative providers can wrap REST APIs, SQL databases, filesystems, or Python scripts under a unified, simple operation contract.
2. **Standardization**: Reuses Nestor's native step runners and transcoders under the hood, maximizing cache efficiency and speed.
3. **Decoupled Security & Configuration**: Connection credentials, authentication headers, and connection pool configurations are locked in the provider boundary, completely hidden from workflow orchestrators.

