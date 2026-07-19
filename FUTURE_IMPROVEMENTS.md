# Nestor Orchestration Engine - Future Improvements Specification

This document details the architectural design and implementation specifications for the new Phase 1 extension stages (Stages 8.1 to 8.4).

---

## Stage 8.1: Subprocess Plugin Async Timeouts & Execution Bounds

### 1. Problem Description
Currently, subprocess plugins spawned via `fork()` / `execvp()` run synchronously or asynchronously without a non-blocking timeout. If a plugin hangs or blocks on I/O indefinitely, it holds lockups on the engine execution scheduler loop.

### 2. Proposed Design
* **Timeout Specification**: Add a `timeout` property to the step schema for plugins (similar to HTTP step timeout parsing).
* **Non-Blocking Poll Loop**:
  * Instead of a blocking `waitpid()`, the execution loop should check running child processes periodically using `waitpid(pid, &status, WNOHANG)`.
  * Track start timestamps of each running subprocess in the active job structure.
  * If the subprocess elapsed time exceeds the specified timeout:
    * Send `SIGTERM` to the child process.
    * Wait up to 500ms; if it does not exit, send `SIGKILL`.
    * Clean up file descriptors, set the step outcome status code to `-4` (`ERR_TIMEOUT`), and transition the job state to `STATE_FAILED`.

---

## Stage 8.2: Concurrent Job Semaphores (Concurrency Throttling)

### 1. Problem Description
For large graphs with high branch factors, Nestor currently launches all ready-to-run jobs in parallel. This can lead to resource starvation, reaching system socket/file descriptor limits, or CPU thrashing.

### 2. Proposed Design
* **Maximum Concurrency Setting**: Support an execution config parameter (e.g. `max_concurrency: N`) at the workflow root.
* **Token Bucket Scheduler**:
  * Implement a simple atomic integer counter in the execution context representing active job tokens.
  * Before promoting a job from `STATE_READY` to `STATE_RUNNING`, verify if `active_jobs_count < max_concurrency`.
  * If the limit is reached, hold the job in the ready queue. As running jobs complete and release their tokens, promote the next ready jobs in sorted order.

---

## Stage 8.3: Step/Job Retry Policies (Transient Error Handling)

### 1. Problem Description
Orchestration steps interacting with external HTTP APIs or legacy plugin scripts can fail due to transient network congestion or temporary service unavailability. Failing the entire workflow immediately requires manual restart.

### 2. Proposed Design
* **Retry Schema**: Extend the Step/Job parser schemas to support:
  ```json
  "retry": {
    "attempts": 3,
    "backoff": "exponential",
    "initial_delay": "1s",
    "max_delay": "10s"
  }
  ```
* **Retry Loop States**:
  * Add `retry_count` and `next_retry_timestamp` fields to the `ActiveJob` structure.
  * Upon step failure (non-zero plugin status or HTTP transport/error code), check if `retry_count < attempts`.
  * If eligible for retry:
    * Set step/job state to a transient `STATE_RETRIES_PENDING`.
    * Calculate the next execution delay (e.g., $initial\_delay \times 2^{retry\_count}$ for exponential backoff).
    * During loop ticks, compare the current time with `next_retry_timestamp` before scheduling the step again.

---

## Stage 8.4: Loop Iteration Memory Compaction & Sub-Arenas

### 1. Problem Description
The chained arena allocator (`Arena`) is strictly deterministic and does not reclaim individual allocations until `arena_destroy()` is called. In very long-running loops or infinite `while` loops, the arena size grows indefinitely.

### 2. Proposed Design
* **Generation/Sub-Arena Scoping**:
  * Introduce a temporary sub-arena for job/loop iteration scopes.
  * At the start of a loop iteration, allocate a sub-arena node off the main Arena.
  * Redirect all transient step execution variables, headers, and intermediate string conversions during that iteration to the sub-arena.
  * When the loop iteration finishes, reset or free the sub-arena block, returning its memory to the pool while keeping the parent workflow-scoped variables intact.
