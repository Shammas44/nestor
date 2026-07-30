# Nestor Workflow Orchestration Engine: Scenario Development Guide

Nestor is a high-performance, memory-optimized workflow execution engine designed to manage complex job dependencies, parallel execution branches, native data transformations, and sandboxed dynamic plugins.

Workflows in Nestor are specified as scenarios in either **JSON** or **YAML** format.

---

## 1. Scenario Structure Overview

Every Nestor scenario file contains four top-level keys:

```json
{
  "version": "2.0.0",
  "name": "My Orchestration Scenario",
  "on": {
    "manual": {}
  },
  "jobs": {
    ...
  }
}
```

* **`version`**: The semver version of the Nestor schema (e.g., `"2.0.0"`).
* **`name`**: A user-friendly descriptor of the workflow.
* **`on`**: Trigger definitions (typically `"manual": {}` for on-demand CLI execution).
* **`jobs`**: An object containing the jobs that form the directed acyclic graph (DAG) of the workflow.

---

## 2. Job Types

Nestor supports six native job types, configured via the `"type"` field under a job:

### 2.1 Task Jobs (`"type": "task"`)
Executes a sequence of steps sequentially.
```json
"run_actions": {
  "type": "task",
  "steps": [
    {
      "id": "step_one",
      "http": { "method": "GET", "url": "https://api.example.com/v1" }
    }
  ]
}
```

### 2.2 Conditional Branching (`"type": "if"`)
Evaluates a condition expression. Routes to the jobs in the `"then"` array if true, or the `"else"` array if false.
```json
"check_status": {
  "type": "if",
  "condition": "${{ inputs.tier = 'enterprise' }}",
  "then": ["premium_deploy"],
  "else": ["standard_deploy"]
}
```

### 2.3 Multi-Case Branching (`"type": "switch"`)
Allows multi-path routing based on sequential condition checks. Matches the first truthy condition or routes to the `"default"` branch.
```json
"route_region": {
  "type": "switch",
  "cases": [
    { "condition": "${{ inputs.region = 'us' }}", "then": ["us_deploy"] },
    { "condition": "${{ inputs.region = 'eu' }}", "then": ["eu_deploy"] }
  ],
  "default": ["fallback_deploy"]
}
```

### 2.4 Concurrency Barriers (`"type": "fork"` and `"type": "join"`)
* **`fork`**: Splits execution into multiple parallel branches concurrently.
* **`join`**: Synchronizes concurrent branches. Supports three join strategies:
  * `"all"`: Wait for all parent branches to complete.
  * `"any"`: Proceed as soon as any single parent branch finishes.
  * `"n_required"`: Proceed once a set number of parent branches finish.

```json
"split_paths": {
  "type": "fork",
  "branches": ["path_a", "path_b"]
},
"merge_paths": {
  "type": "join",
  "depends_on": ["path_a", "path_b"],
  "spec": {
    "strategy": "any"
  }
}
```

### 2.5 Loops (`"type": "loop"`)
Supports three loop types:
* **`for_each`**: Iterates over a list of items, mapping the active element to `${{ item }}`.
* **`while`**: Repeats steps while the condition evaluates to true, exposing `${{ index }}`.
* **`stream_chunk`**: Iterates over a streaming dataset file chunk-by-chunk using a constant-memory SAX parser. Exposes the active page/chunk of records as `${{ chunk }}` (an array of records) and the iteration count as `${{ index }}`.

```json
"process_list": {
  "type": "loop",
  "loop_type": "for_each",
  "items": "${{ inputs.planet_ids }}",
  "steps": [
    {
      "id": "query",
      "http": { "method": "GET", "url": "https://api.com/planets/${{ item }}" }
    }
  ]
}
```

Example of `stream_chunk` loop:
```json
"process_chunks": {
  "type": "loop",
  "loop_type": "stream_chunk",
  "source": "jobs.fetch_dataset.steps.download.stream",
  "items": "data.id",
  "chunk_record_limit": 1000,
  "steps": [
    {
      "id": "transform_chunk",
      "http": {
        "method": "POST",
        "url": "http://127.0.0.1:8080/api/transform",
        "body": {
          "records": "${{ chunk }}"
        }
      }
    }
  ]
}
```

### 2.6 Native JSONata Transforms (`"type": "transform"`)
Performs inline data mappings using JSONata expressions, publishing the result under the job's outcomes as `result` and `outputs`.
```json
"format_data": {
  "type": "transform",
  "spec": {
    "expression": "${{ 'Processed value: ' & jobs.upstream_job.steps.step_id.outputs.val }}"
  }
}
```

---

## 3. Scoping & Variables (Public vs. Private)

Nestor supports declaring scoped variables at the workflow root, job, or step level to calculate values dynamically using expression bindings.

```json
{
  "version": "2.0.0",
  "name": "My Orchestration Scenario",
  "on": { "manual": {} },
  "variables": [
    {
      "name": "global_const",
      "expression": "'hello'"
    }
  ],
  "jobs": {
    "job1": {
      "type": "task",
      "variables": [
        {
          "name": "job_var",
          "expression": "global_const & ' world'"
        }
      ],
      "steps": [
        {
          "id": "step_one",
          "variables": [
            {
              "name": "step_var",
              "expression": "job_var & '!'",
              "visibility": "public"
            }
          ],
          "http": { "method": "GET", "url": "https://api.example.com" }
        }
      ]
    }
  }
}
```

### 3.1 Variable Visibility & Scoping
Variables accept a `"visibility"` property (`"private"` or `"public"`), defaulting to `"private"`:
* **`private`**: The variable is only accessible locally within the job/step's execution stack. It is evaluated in-memory and is excluded from SQLite DB state serialization to optimize database size.
* **`public`**: The variable is exposed to downstream execution nodes, serialized, and published under the job outcomes at `jobs.<job_id>.outputs.<var_name>`.

### 3.2 Cycle Checks & Name Validation
* **Cycle Check**: During compilation, Nestor topologically sorts variable dependencies. Any cyclic references (e.g. `var_a` referencing `var_b` which references `var_a`) will fail compilation with `ERR_CYCLIC_DEP`.
* **Name Validation**: Variable names must follow standard naming rules (snake_case/camelCase: must start with an alpha character or underscore, followed only by alphanumeric characters or underscores). Invalid names fail compilation.

---

## 4. Step Definitions

Under `"type": "task"` jobs, you declare an array of steps. There are two primary step configurations:

### 4.1 HTTP Steps
Performs non-blocking HTTP requests using `curl_multi`.
* **Properties**: `method`, `url`, `headers`, `body`, `timeout`, `stream`, `chunk_size`
  * **`stream`**: A boolean flag (`true`/`false`). When enabled, the HTTP response payload is streamed directly to a temporary file on disk (`.nestor_stream_<step_id>.json`) in chunked blocks of memory to prevent storing large DOMs in RAM.
  * **`chunk_size`**: The buffer size in bytes for reading stream data blocks (default is `65536` bytes).
* **Retries & Backoff**: Optional automatic retries with backoff delays.

```json
{
  "id": "fetch_report",
  "http": {
    "method": "POST",
    "url": "https://api.com/report",
    "headers": {
      "Content-Type": "application/json"
    },
    "body": {
      "filter": "active"
    },
    "timeout": "5s",
    "retries": 3,
    "backoff_factor": 2
  }
}
```

### 4.2 Plugin Steps (`uses`)
Invokes C SDK libraries or standalone executables.
* **`uses`**: The name of the plugin (resolved dynamically) or a file path.
* **`sandboxed`**:
  * `false` (Default): Loads trusted dynamic libraries (`.so`/`.dylib`) inline in the main engine process via C SDK ABI function entry points for ultra-fast, zero-overhead execution.
  * `true`: Executes untrusted plugins inside a secure subprocess wrapper (`nestor-plugin-runner`) to isolate segment faults, memory issues, or hung threads.

```json
{
  "id": "run_plugin",
  "uses": "my_dynamic_plugin",
  "sandboxed": true,
  "with": {
    "arg_one": "value_one"
  }
}
```

### 4.3 Step-Level Outcome Projection (Memory Minimization)
Steps can define an `outputs` block containing key-value mappings. When output projection is specified, Nestor evaluates the JSONata expressions against the step outcome, saves the projected fields, and releases the raw response body from the memory arena immediately.
```json
{
  "id": "fetch_data",
  "uses": "test_dynamic_plugin",
  "outputs": {
    "projected_status": "outputs.status",
    "val": "outputs.computed_val"
  }
}
```

### 4.4 Provider Steps (`provider`)
Executes an operation exposed by a globally configured provider, dynamically routing arguments and configurations to Nestor's plugin runners.
```json
{
  "id": "run_prov",
  "provider": "my_provider.operation_name",
  "args": {
    "dummy_param": "abc"
  }
}
```

### 4.5 Step-Level `on_error` Fallbacks
Steps can define a fallback outcome object to be returned if the step fails (e.g., timeout or non-2xx HTTP status), allowing the pipeline to proceed gracefully.
```json
{
  "id": "fetch_metadata",
  "http": { "method": "GET", "url": "https://api.com/metadata" },
  "on_error": {
    "fallback": { "status": "offline", "data": [] }
  }
}
```

---

## 5. Global Provider Configurations & Session Reuse

Global provider configurations centralize settings (TCP/TLS session pools, database pools, credentials, and api keys) at the workflow level. 

Resolved configurations are automatically injected into the execution context under the `providers.<provider_id>` key, making them accessible to step outcome projections, variables, or dynamic plugins via `host_api->get_variable`.

```json
{
  "version": "2.0.0",
  "name": "My Orchestration Scenario",
  "on": { "manual": {} },
  "providers": {
    "postgres": {
      "connection_string": "postgresql://user:pass@host:5432/db",
      "max_connections": 5
    }
  },
  "jobs": {
    "sync": {
      "type": "task",
      "variables": [
        {
          "name": "db_conn",
          "expression": "providers.postgres.connection_string",
          "visibility": "public"
        }
      ],
      "steps": [
        {
          "id": "insert_row",
          "provider": "postgres.insert",
          "args": {
            "table": "users",
            "record": { "id": 1, "name": "Alice" }
          }
        }
      ]
    }
  }
}
```

---

## 6. Fine-Grained Edge Dependencies

When a job declares dependencies using `"depends_on"`, it can specify conditions to refine exactly when the edge is considered satisfied:

* **`onSuccess`**: Triggered only if the parent job completed successfully (exit code 0).
* **`onFailure`**: Triggered if the parent job failed (non-zero exit code or crashed).
* **`onSkip`**: Triggered if the parent job was skipped.
* **`onCompletion`**: Triggered regardless of the parent job's final status.

```json
"depends_on": [
  {
    "job": "build_artifact",
    "conditions": ["onSuccess"]
  },
  {
    "job": "security_scan",
    "conditions": ["onFailure", "onSkip"]
  }
]
```

---

## 7. Caching and Observability Features

### 7.1 SQLite-Backed Job Cache
Nestor automatically caches task outcomes (status codes, outputs, response bodies) in a SQLite DB file. It uses SHA-256 keys derived from the job type, specification parameters, inputs, and active environment.
* **Cache Revalidation**: Supports HTTP revalidation. If a cached response has `ETag` or `Last-Modified` headers, subsequent runs automatically send `If-None-Match` or `If-Modified-Since` headers to the server. If the server returns `304 Not Modified`, Nestor restores the cached payload.
* **Eviction Policies**: Employs TTL (Time-To-Live) verification and LRU (Least-Recently Used) eviction to automatically prune old entries.

### 7.2 Observability & Redaction
Employs a high-speed Aho-Corasick keyword matching trie to identify sensitive keys (e.g. `secrets` block) on the fly, redacting them (`***`) automatically in all stderr/stdout stream printouts and workspace outputs before writing files.

### 7.3 Data Sources vs. Resources Split
To optimize execution paths and prevent cache corruption, Nestor classifies pipeline steps into:
* **Data Sources (Read-Only)**: Side-effect-free steps (e.g. GET/HEAD HTTP requests or query plugins). These are cached in the SQLite DB and executed in parallel by the scheduler.
* **Resources (Mutative)**: Mutative actions (e.g. POST, PUT, DELETE, PATCH HTTP requests or mutative plugins). These bypass caching completely and execute serially (blocking other executions).

---

## 8. Deterministic Graph Boundaries

Nestor enforces strict safety guarantees regarding workflow execution and concurrent process lifecycles through deterministic graph boundaries:

### 8.1 Start Boundary (Single Entry Point)
A workflow must declare a single starting job:
* **Implicit Start**: By default, if exactly one job has `dependency_count == 0` (no `depends_on`), the compiler infers it as the entry point. If multiple root jobs exist and none are marked `"start": true`, compilation fails.
* **Explicit Start**: Setting `"start": true` on a job forces it to be the entry point. Only **one** job in a scenario may be marked as start. If multiple are marked, compilation fails.

```json
"initialize_pipeline": {
  "type": "task",
  "start": true,
  "steps": [...]
}
```

### 8.2 End Boundary & Custom Return Expressions
A workflow can designate terminal exit jobs using `"end"` or `"return"` properties:
* **`end`** (`boolean`): Designates the job as a terminal exit point. When it completes successfully, the scheduler halts execution immediately and returns the job's outcomes under the top-level `"outputs"` key in the final context.
* **`return`** (`expression` / `object`): Designates the job as a terminal exit point and evaluates a custom JSONata expression or maps a nested JSON structure (resolving placeholders) to be bound to the top-level `"outputs"` key.
* **CLI Return Formatting**: By default, if the workflow hits an exit/return job, Nestor outputs *only* the final evaluated return value (the `"outputs"` key) on stdout.
  * To output the full workflow outcomes (including `jobs` outcomes list, `status`, and `outputs` metadata) for debugging, pass the `--debug`, `-d`, or `--verbose` command-line flags, or set the `NESTOR_DEBUG=true` environment variable.

```json
"evaluate_fleet": {
  "type": "transform",
  "spec": {
    "expression": "10 + 20"
  },
  "return": {
    "result_val": "${{ jobs.evaluate_fleet.result }}",
    "status_str": "completed"
  }
}
```

### 8.3 Join Dominance Concurrency Check
To prevent dangling concurrent branches or race conditions, any path traversing a `fork` job node **must** pass through a `join` job node before hitting any exit node (where `is_end == true` or a `return` expression is defined).
If any concurrent branch of a `fork` reaches an exit job without a synchronizing `join`, compilation fails.

---

## 9. State Backends & Execution Locking

Nestor supports state serialization to backend storage (defaulting to local `.tfstate` files).
* **Execution Suspension**: When hitting a `wait_signal` or timer node, the engine serializes the active stack frames, variables, scheduler queue, and bitstack states to a `.nestor.tfstate` file and halts execution.
* **Execution Locks**: To prevent concurrent execution conflicts, Nestor acquires a `.tfstate.lock` file on the workflow name at startup. If the lock is already held, Nestor exits with `ERR_LOCKED`.
* **Resuming state**: Upon receiving a trigger signal, Nestor verifies lock integrity, reads the state file, and resumes execution from the suspension boundary.

---

## 10. Declarative YAML Providers

Declarative providers allow developers to wrap REST APIs, databases, or plugins into reusable operations without compiling C code.
* Provider contracts are loaded dynamically from the `./providers` directory at engine startup.
* Operations are declared in YAML, mapping parameters and input types to a target plugin (`uses`) evaluated against a custom sub-context.

### 10.1 Declarative Provider Example (`providers/stripe.yaml`)
```yaml
provider: stripe
description: Stripe payment gateway wrapper
configuration:
  api_key: "${{ secrets.stripe_key }}"

operations:
  charge_customer:
    uses: stripe.charge
    args:
      amount: "${{ inputs.amount }}"
      currency: "usd"
      customer: "${{ inputs.customer_id }}"
    inputs:
      amount: { type: number, required: true }
      customer_id: { type: string, required: true }
    outputs:
      charge_id: "body.id"
      status: "body.status"
```

