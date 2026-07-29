# Specification: Nestor Virtual Machine (NVM) & Bytecode Compiler

- **Spec Identifier:** SPEC-NVM-001
- **Status:** APPROVED
- **Version:** 1.0.0

---

## 1. Executive Summary & Design Goals

This document specifies the technical design, security controls, and runtime requirements for the **Nestor Virtual Machine (NVM)** and its accompanying **Bytecode Compiler**. 

The goal of the Nestor VM is to execute declarative workflows with minimal latency, sub-microsecond cold starts, and a constant memory footprint. Instead of parsing text files and traversing ASTs recursively during execution, the runtime compiles workspaces into a binary bytecode format (`.nbc`) that maps directly into memory.

---

## 2. Requirements Analysis

### 2.1 Performance & Latency Constraints
*   **Zero-Copy Execution**: The VM must resolve variables, headers, and payloads by creating pointer views into the memory-mapped constant pool or HTTP/Database buffers without allocating heap copies.
*   **Sub-Microsecond Cold Starts**: Loading and starting a workflow must take less than 1 microsecond. This is achieved by using the `mmap` system call to map the `.nbc` file directly, allowing the VM to cast structures instantly without parsing or scanning.
*   **Constant Memory Scaling**: The execution of loops and streaming steps must operate within a fixed memory threshold, using compaction to clean intermediate data at frame boundaries.

### 2.2 Functional Requirements
*   **Dynamic Plugin loading**: The VM must coordinate calling dynamic library providers (`.so`/`.dylib` plugins) using standard ABI signatures.
*   **Nested Stack Frames**: Hierarchical sub-workflow invocations must push isolated variable scopes onto the VM's call stack.
*   **JSONata Integration**: The VM must evaluate `${{ ... }}` expressions by binding JSONata engine invocations directly to stack operations.

---

## 3. Security & Sandboxing Analysis

Executing compiled bytecode poses unique security challenges compared to AST interpretation. The VM must mitigate these risks at the compiler, instruction decoder, and runtime boundaries.

```
                  UNTRUSTED BYTECODE FILE (.nbc)
                               │
                               ▼
            [ Stage 1: Static Binary Verification ]
            - Magic number / Header checks
            - Bounds verification (Constant Pool vs Instructions)
            - Instruction pointer bounds checking
                               │
                               ▼
                  [ Stage 2: VM Execution ]
            - Opcode validation
            - Stack overflow & underflow checks
            - Sandbox isolated memory frames
```

### 3.1 Memory Corruption & Out-of-Bounds Mitigations
*   **Static Binary Verification**: Before execution begins, the VM verifies the `.nbc` binary header structure. It asserts that all instruction offsets and constant pool bounds are valid.
*   **Instruction Pointer Boundary Checking**: Every jump instruction (`OP_JUMP`, `OP_JUMP_IF_FALSE`, etc.) must target an offset strictly within the bytecode instruction segment bounds. Invalid jump offsets immediately abort execution returning `ERR_VM_INVALID_JUMP`.
*   **Stack Underflow & Overflow Protections**: The evaluation stack is allocated with a strict limit. The instruction decoder checks stack boundaries before pushing or popping values to prevent stack overflow/underflow exploits.

### 3.2 Bytecode Tampering Defenses
*   **Integrity Verification**: To prevent malicious alterations to deployed workflows on disk, Nestor supports signing bytecode binaries. The VM verifies the digital signature of the `.nbc` binary file against a configured public key before loading it.
*   **Opcodes Validation**: The VM execution loop only executes supported opcodes. Unrecognized opcodes trigger immediate termination with `ERR_VM_ILLEGAL_INSTRUCTION`.

### 3.3 Provider Sandbox Isolation
*   **Memory Boundaries**: Dynamic plugins (providers) are executed inside separate process boundaries when `"sandboxed": true` is set.
*   **Host API Constraints**: Sandboxed providers query host variables over isolated Unix IPC sockets, preventing direct access to the parent process's memory space or secrets directory.

---

## 4. C Implementation Best Practices

To comply with Nestor's strict memory safety rules, the NVM runtime implementation must adhere to these C programming practices:

### 4.1 Strict Arena Allocation Policy
*   The VM execution loop must **never** call standard heap allocators (`malloc`, `free`, `realloc`, `calloc`).
*   All dynamic values, evaluation stacks, and sub-workflow frames must be allocated from the active `Arena` passed to the VM runtime.
*   The Call Stack frame list uses an intrusive layout, embedding frame pointers directly within the `ActiveJobFrame` structure to avoid wrapping allocation overhead.

### 4.2 Data Alignment & Memory Mapping
*   **8-Byte Alignment**: Ensure all constant pool offsets and bytecode structures are padded to 8-byte boundaries. This avoids alignment faults when executing on architectures that enforce aligned accesses (like ARM64).
*   **Direct Casting**: The VM casts mapped bytecode slices directly to C structs (e.g. casting the header segment to `NVMHeader*`). No parsing is permitted during runtime initialization.
