# Nestor Orchestration Engine - Plugin Specification & SDKs

This document describes Nestor's plugin model capabilities, areas of improvement, and provides instructions/code examples for writing plugins using the new C and Python SDKs.

---

## 1. Current Plugin Execution Model
Nestor plugin steps are defined with the `uses` property pointing to an executable binary or script:
* **Inputs**: The engine writes the step's `with` arguments to a temporary JSON file and sets the path in the `NESTOR_PLUGIN_INPUT` environment variable.
* **Environment**: The host environment context (mapped under `env` in the workflow) is passed as a serialized JSON string in the `NESTOR_ENV` environment variable.
* **Outputs**: The plugin emits a structured JSON or YAML string to `stdout`. Nestor captures `stdout`, redacts any secrets using the Aho-Corasick DFA, and parses the content into the step outcomes map.

---

## 2. Planned Improvements
1. **Separate Stderr Collection**:
   Currently, the engine redirects the child process `stderr` to the same stdout pipe:
   ```c
   dup2(pipefd[1], STDERR_FILENO); // Redirection in runner.c
   ```
   If a plugin emits log messages or warnings to `stderr`, this text gets concatenated with the stdout payload, causing the parser to fail decoding the structured JSON/YAML.
   * *Fix*: Redirect `stderr` to a separate logging descriptor, or let it flow directly to the parent process's `stderr`/observability stream.
2. **Dynamic Context Queries**:
   Allow plugins to query variables from the parent process on demand over a UNIX socket or IPC pipe rather than pre-serializing and passing all values beforehand.

---

## 3. C Plugin SDK Usage Example
Include `#include "nestor_plugin.h"` at the top of your custom C plugin.

### Code Example (`plugins/my_custom_plugin.c`)
```c
#include "nestor_plugin.h"

int main(void) {
    char *input_json = np_get_input_json();
    if (!input_json) {
        np_error(1, "Failed to read input variables");
    }

    // Process variables (e.g. using a JSON parser library)
    // ...

    // Output success payload
    np_success("{\"status\": \"completed\", \"code\": 200}");
    return 0;
}
```

---

## 4. Python Plugin SDK Usage Example
Import `NestorPlugin` from `nestor_plugin.py` at the top of your Python script.

### Code Example (`plugins/my_custom_plugin.py`)
```python
#!/usr/bin/env python3
from nestor_plugin import NestorPlugin

def main():
    inputs = NestorPlugin.get_input()
    env = NestorPlugin.get_env()

    # Access step variables easily
    user_id = inputs.get("user_id", "default_user")
    
    # Emit result payload
    NestorPlugin.success({
        "status": "success",
        "processed_user": user_id,
        "cluster": env.get("CLUSTER_NAME", "local")
    })

if __name__ == '__main__':
    main()
```

---

## 5. Dynamic Shared-Library Plugin SDK (Phase 1 Extension)
Nestor supports compiled shared libraries (`.so`/`.dylib`) using a stable ABI and capability negotiation. 

### Host API Function Pointers
The dynamic library registers by receiving function pointers to host-implemented operations:
*   `void* (*alloc)(void *arena_ptr, size_t size)` - Allocate memory off the host-managed job `Arena`.
*   `void (*log)(int level, const char *message)` - Write log output.
*   `const char* (*get_variable)(void *ctx_ptr, const char *json_path)` - Resolve context variables.
*   `int32_t (*set_output)(void *ctx_ptr, const char *key, const char *json_val)` - Set step/job outputs.

### Sandboxed Subprocess Isolation
*   **Property**: `"sandboxed": true`
*   **Behavior**: When configured, the host spawns a subprocess wrapper (`nestor-plugin-runner`) to isolate unverified plugins, capturing crashes, timeouts, and leaks without impacting the main engine's daemon execution.

