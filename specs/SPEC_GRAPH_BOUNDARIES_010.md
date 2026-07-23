# Nestor Orchestration Engine: Deterministic Graph Boundaries Spec
**Document ID:** Nestor-RFC-010  
**Status:** APPROVED  
**Author:** Core Architecture Group  
**Subject:** Technical Specification for Job-Property Start and End Boundaries

---

## 1. Overview & Architectural Goals

Nestor enforces strict safety guarantees regarding workflow execution and concurrent process lifecycles. This specification defines job-level properties to explicitly declare starting and ending boundaries in a Directed Acyclic Graph (DAG), enabling compact configurations without the need for boilerplate structural nodes.

The goals of these boundaries are:
*   **Strict Concurrency Safety**: Prevent orphaned/dangling child processes or race conditions in parallel paths.
*   **Boilerplate Reduction**: Allow any standard job (e.g. `task` or `transform`) to act as the entry or exit point of the workflow.
*   **Deterministic Outputs**: Provide clear, path-specific return expressions that terminate the execution immediately.

---

## 2. Technical Specification

### 2.1 Job-Level Boundary Properties

Any job node in the scenario configuration can declare boundary properties:
*   **`start`** (`boolean`, optional): Explicitly designates a job as the entry point of the workflow.
*   **`end`** (`boolean`, optional): Explicitly designates a job as a terminal exit point.
*   **`return`** (`expression` / `object`, optional): Designates the job as a terminal exit point and defines a custom JSONata mapping for the workflow's final return value.

---

### 2.2 Entry Point (Start Validation)

*   **Implicit Start**: By default, if exactly one job has `dependency_count == 0` (no `depends_on`), the compiler infers it as the starting entry point. If multiple root jobs exist and none are marked `"start": true`, compilation fails.
*   **Explicit Start**: Setting `"start": true` on a job forces it to be the entry point. Only **one** job in a scenario may be marked as start. If multiple are marked, compilation fails.

```yaml
jobs:
  initialize_pipeline:
    type: task
    start: true # Explicitly marked starting job
    steps: [...]
```

---

### 2.3 Exit Points & Join Dominance (End Validation)

*   **Termination Behavior**: When a job marked with `"end": true` or `"return"` completes execution successfully:
    1. The scheduler immediately stops the workflow run.
    2. Any active polling operations or subprocesses on other branches are cancelled.
    3. The workflow returns the job's outputs (if `"end": true` is set) or the evaluated JSONata expression mapping (if `"return"` is set).
*   **Join Dominance Check**: If any path leading to an exit job (marked `"end": true` or `"return"`) traverses a `fork` job, all parallel branches from that fork **must** converge at a `join` job before that exit job is executed. This prevents race conditions and process truncation on concurrent branches.

```mermaid
graph TD
    subgraph INVALID (Dangling Concurrency)
        A1[Job A: Fork] --> B1[Job B]
        A1 --> C1[Job C: End]
    end

    subgraph VALID (Synchronized Concurrency)
        A2[Job A: Fork] --> B2[Job B]
        A2 --> C2[Job C]
        B2 --> D2[Job D: Join]
        C2 --> D2
        D2 --> E2[Job E: End]
    end
```

---

### 2.4 End Job Examples

#### Example A: Simple `end: true` (Return Job Outputs)
```yaml
  evaluate_fleet:
    type: transform
    depends_on: ["join_fleet"]
    expression: ${{ $number(jobs.fetch_xwing.steps.get_xwing.body.passengers) + $number(jobs.fetch_falcon.steps.get_falcon.body.passengers) }}
    end: true # Terminate and return 'outputs: 6'
```

#### Example B: Custom Mapped `return` Block
```yaml
  process_report:
    type: task
    depends_on: ["collect_metrics"]
    steps: [...]
    return:
      total_passengers: ${{ jobs.evaluate_fleet.result }}
      run_status: "completed"
```
