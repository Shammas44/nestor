# Specification: Nestor Integration Runtime Future Paradigms

This specification defines the proposed architectural features for the Nestor Integration Runtime (IR) based on Terraform design philosophies. These enhancements focus on contract encapsulation, memory optimization, session reuse, and stateful execution.

---

## Developer Personas & The Provider Boundary

To maintain separation of concerns and scaling efficiency, Nestor divides the integration development lifecycle into two distinct personas, separated by a strict YAML contract boundary:

### 1. Provider Developer
* **Role**: Low-level integration engineering. Provider developers write C/C++ plugins (dynamic libraries `.so` / `.dylib`) that handle authentication, low-level network protocols, connection pools, and database drivers.
* **APIs**: They work with the low-level Provider SPI (`nestor_provider_execute`), `Arena` memory allocators, and native system capabilities.
* **Deliverable**: Compiles binary plugins and publishes a **YAML Provider Contract** (schema file) declaring the exposed operations, required inputs, and return output structures.

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

## 8. Declarative Providers (No-Code YAML REST Mapping)

For standard web APIs, Nestor allows Provider Developers to define providers **strictly in YAML/JSON** without writing compiled C code. 

### 8.1 Mechanics
The provider manifest wraps a base API and maps specific YAML operations to HTTP requests (methods, paths, payload transcoding, and header injections) under the hood. The core engine parses this definition and acts as the runtime provider executor.

### 8.2 Syntax Example (`providers/stripe.yaml`)
```yaml
provider: stripe
description: Declarative Stripe Integration
configuration:
  base_url: "https://api.stripe.com/v1"
  headers:
    Authorization: "Bearer ${{ secrets.stripe_key }}"
    Content-Type: "application/json"

operations:
  create_customer:
    method: POST
    path: "/customers"
    inputs:
      email: { type: string, required: true }
      name: { type: string, required: true }
    outputs:
      stripe_id: "body.id"
      created_at: "body.created"
```

### 8.3 Benefits
1. **No compilation required**: Zero-compilation overhead for connecting simple HTTP REST web APIs.
2. **Standardization**: Reuses the core HTTP transport (`transport.c`) and transcoders (`transcoder.c`) natively.
3. **Decoupled Security**: Credentials and authorization tokens are injected at the provider boundary via configuration blocks, keeping them fully isolated from the high-level workflow files.

