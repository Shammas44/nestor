# Nestor Orchestration Engine: Overview

Nestor is a high-performance, memory-optimized workflow execution engine written from scratch in C. It is designed to manage complex job dependencies, parallel execution branches, native data transformations, and sandboxed dynamic plugins.

---

## 1. Rationale & Design Philosophy

Backend integrations are frequently implemented via boilerplate "glue code" microservices. These microservices often consume significant resources (high latency, slow startup, and large memory footprints) because they rely on interpreted runtimes (like Node.js, Python, or Go). 

Nestor was built to replace these boilerplate microservices with **declarative workflows** executed on top of a low-overhead, memory-optimized C engine.

### The "Terraform for Backend Integrations" Analogy
Nestor functions similarly to HashiCorp Terraform, but executes backend integrations instead of cloud resources:
*   **Declarative Scenarios**: Workflows are declared using simple, structure-validated YAML/JSON files.
*   **Provider Model**: Operations (like HTTP requests or database commands) are mapped via reusable providers.
*   **Validation & Plan**: Nestor validates the execution graph (checking schemas, inputs/outputs, and contract boundaries) before execution begins.
*   **State Control**: Active execution runs are tracked and serialized to a `.tfstate` backend to handle locks and gate suspensions.

---

## 2. Core Architectural Goals

Nestor optimizes for the following technical goals:
*   **Zero-Copy Execution**: Variable resolution, payload transformations, and plugin parameters reference slices of the original input buffer directly using length-delimited string views, avoiding standard library string copying.
*   **Deterministic Memory Management**: Nestor operates strictly within a custom **Chained Arena Allocator**; it never calls standard library allocators (`malloc`, `free`, `realloc`, `calloc`) during workflow execution to prevent heap fragmentation and leakages.
*   **Sub-Microsecond Cold Starts**: For production deployments, workspaces can be compiled to a serialized binary bytecode format (`.nbc`) that maps directly into memory via `mmap`, avoiding text parsing latency during startup.
*   **Execution Safety**: Untrusted plugins are sandboxed in isolated subprocess boundaries, and sensitive credentials/keys are redacted on-the-fly from observability streams.
