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

## 3. Step Definitions

Under `"type": "task"` jobs, you declare an array of steps. There are two primary step configurations:

### 3.1 HTTP Steps
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

### 3.2 Plugin Steps (`uses`)
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

---

## 4. Fine-Grained Edge Dependencies

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

## 5. Caching and Observability Features

### 5.1 SQLite-Backed Job Cache
Nestor automatically caches task outcomes (status codes, outputs, response bodies) in a SQLite DB file. It uses SHA-256 keys derived from the job type, specification parameters, inputs, and active environment.
* **Cache Revalidation**: Supports HTTP revalidation. If a cached response has `ETag` or `Last-Modified` headers, subsequent runs automatically send `If-None-Match` or `If-Modified-Since` headers to the server. If the server returns `304 Not Modified`, Nestor restores the cached payload.
* **Eviction Policies**: Employs TTL (Time-To-Live) verification and LRU (Least-Recently Used) eviction to automatically prune old entries.

### 5.2 Observability & Redaction
Employs a high-speed Aho-Corasick keyword matching trie to identify sensitive keys (e.g. `secrets` block) on the fly, redacting them (`***`) automatically in all stderr/stdout stream printouts and workspace outputs before writing files.

---

## 6. Deterministic Graph Boundaries

Nestor enforces strict safety guarantees regarding workflow execution and concurrent process lifecycles through deterministic graph boundaries:

### 6.1 Start Boundary (Single Entry Point)
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

### 6.2 End Boundary & Custom Return Expressions
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

### 6.3 Join Dominance Concurrency Check
To prevent dangling concurrent branches or race conditions, any path traversing a `fork` job node **must** pass through a `join` job node before hitting any exit node (where `is_end == true` or a `return` expression is defined).
If any concurrent branch of a `fork` reaches an exit job without a synchronizing `join`, compilation fails.

