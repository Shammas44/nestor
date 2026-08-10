# Nestor VM Instruction Set Architecture (ISA)

---

## 1. Virtual Machine Execution Model

Nestor VM (NVM) is a stack-based virtual machine. It executes compiled bytecode instructions sequentially or concurrently by operating on two primary stack structures, a local variables environment, and a data constant pool.

### 1.1 Virtual Machine State
The NVM execution context maintains the following register and state structures:
*   **PC (Program Counter)**: Pointer offset targeting the active instruction byte within the code segment.
*   **SP (Stack Pointer)**: Index targeting the active top of the evaluation stack.
*   **Evaluation Stack**: A bounded array of `Jsonv_Value` structures used for operation arguments, binary expressions, and temporary values.
*   **Call Stack**: A stack of `NVMCallFrame` records tracking hierarchical sub-workflow entries, return Program Counters, and caller variable frame pointers.
*   **Constant Pool**: A read-only lookup table of static strings and scalar values loaded from the bytecode header.
*   **Local Variables**: Scoped frame mappings binding variable IDs to evaluated stack values.

```
                    NVM RUNTIME STATE
  ┌──────────────────────┐    ┌──────────────────────┐
  │      Call Stack      │    │   Evaluation Stack   │
  ├──────────────────────┤    ├──────────────────────┤
  │ Frame 2 (sub_wf)     │    │ [1]  "price_basic"   │ <── SP
  ├──────────────────────┤    ├──────────────────────┤
  │ Frame 1 (main_wf)    │    │ [0]  "stripe_id_12"  │
  └──────────┬───────────┘    └──────────────────────┘
             │
             ▼
  ┌──────────────────────┐    ┌──────────────────────┐
  │ Program Counter      │    │    Constant Pool     │
  ├──────────────────────┤    ├──────────────────────┤
  │ PC: 0x00A8           │    │ [0]  "id"            │
  └──────────────────────┘    └──────────────────────┘
```

---

## 2. Binary Bytecode Format (`.nbc`)

A compiled Nestor workflow is serialized as a contiguous binary file structured as follows:

```
┌─────────────────────────────────────────────────────────────┐
│ Header: Magic (4B) | Version (2B) | Flags (2B)              │
├─────────────────────────────────────────────────────────────┤
│ Asymmetric Ed25519 Signature Block (64B)                    │
├─────────────────────────────────────────────────────────────┤
│ Offset Table: Constant Pool | Workflow Table | Code Segment │
├─────────────────────────────────────────────────────────────┤
│ Constant Pool: Length-prepended static strings/keys         │
├─────────────────────────────────────────────────────────────┤
│ Workflow/Function Table: Labeled entry points & metadata   │
├─────────────────────────────────────────────────────────────┤
│ Instruction Segment: Opcode (1B) + Operands (var length)    │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 The Header Struct
```c
#pragma pack(push, 1)
typedef struct {
  char magic[4];               // Must be "NEST"
  uint16_t version;            // Target Nestor version (e.g. 4)
  uint16_t flags;              // Bit 0: Signed, Bit 1: Debug symbols present
  uint8_t signature[64];       // Ed25519 asymmetric signature of the binary
  
  uint32_t const_pool_offset;  // Offset to Constant Pool
  uint32_t const_pool_count;   // Number of entries in Constant Pool
  
  uint32_t wf_table_offset;    // Offset to Workflow/Function Table
  uint32_t wf_table_count;     // Number of workflows/functions bundled
  
  uint32_t code_offset;        // Offset to Instruction Segment
  uint32_t code_size;          // Size in bytes of Instruction Segment
} NVMHeader;
#pragma pack(pop)
```

---

## 3. Instruction Set & Opcode Reference

All instructions consist of a 1-byte **Opcode** followed by zero or more operands. Operands are represented as standard network-byte-order integers (big-endian).

### 3.1 Operations Reference Table

| Opcode | Mnemonic | Operands | Stack Effect | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `OP_NOP` | None | None | No operation. |
| `0x01` | `OP_PUSH_CONST` | `uint32_t const_idx` | `[] -> [val]` | Pushes a value from Constant Pool at `const_idx` onto the evaluation stack. |
| `0x02` | `OP_POP` | None | `[val] -> []` | Pops and discards the top value off the evaluation stack. |
| `0x03` | `OP_RESOLVE` | `uint32_t path_idx` | `[] -> [val]` | Evaluates a JSONata expression string resolved at `path_idx` against the active context. |
| `0x04` | `OP_JUMP` | `uint32_t offset` | None | Sets Program Counter (PC) directly to `offset`. |
| `0x05` | `OP_JUMP_IF_FALSE`| `uint32_t offset` | `[cond] -> []` | Pops top evaluation stack value. If it is falsy, sets PC to `offset`. |
| `0x06` | `OP_CALL_PROVIDER`| `uint32_t prov_idx` | `[args...] -> [res]` | Invokes the provider function block at `prov_idx` with stack arguments. |
| `0x07` | `OP_CALL_WORKFLOW`| `uint32_t entry_pc` | `[args...] -> [res]` | Pushes call frame, isolates context, and jumps to workflow at `entry_pc`. |
| `0x08` | `OP_RETURN` | None | `[res] -> [res]` | Returns from sub-workflow, restoring parent PC and call stack frame. |
| `0x09` | `OP_FORK` | `uint8_t count`, `uint32_t offsets[]` | None | Spawns `count` parallel threads starting at specified code offsets. |
| `0x0A` | `OP_JOIN` | `uint8_t strategy`, `uint8_t count` | None | Suspends thread until `count` fork siblings complete matching `strategy`. |
| `0x0B` | `OP_LOAD_ENV` | `uint32_t key_idx` | `[] -> [val]` | Loads environment variables or secrets injected at runtime by the host VM. |
| `0x0C` | `OP_STORE_VAR` | `uint32_t key_idx` | `[val] -> []` | Binds top stack value to local variables scope mapping at `key_idx`. |

---

## 4. Execution Logic Patterns

### 4.1 Loops (NODE_LOOP)
Loops (such as `while` or `for_each`) are compiled into low-level conditional branches:
```assembly
  ; Initial loop counters
  OP_PUSH_CONST  0                ; Initial index=0
  OP_STORE_VAR   1                ; Save to index slot
  
.loop_start:
  ; Evaluate condition index < count
  OP_PUSH_CONST  10               ; count limit
  OP_RESOLVE     "index < limit"  ; Resolve condition
  OP_JUMP_IF_FALSE .loop_end      ; Exit loop if false
  
  ; Execute Loop Body
  OP_CALL_PROVIDER "fetch_item"
  
  ; Increment loop index
  OP_RESOLVE     "index + 1"
  OP_STORE_VAR   1
  OP_JUMP        .loop_start      ; Jump back
  
.loop_end:
```

### 4.2 Forks (NODE_FORK) and Joins (NODE_JOIN)
Parallel tasks use thread management:
```assembly
  ; Spawn branches concurrently
  OP_FORK        2, 0x00000018, 0x00000030
  OP_JUMP        0x00000048      ; Main thread jumps past branches to join
  
  ; Branch thread 1
  0x00000018: OP_CALL_PROVIDER "task1"
  0x0000001F: OP_RETURN
  
  ; Branch thread 2
  0x00000030: OP_CALL_PROVIDER "task2"
  0x00000037: OP_RETURN
  
  ; Main thread Join barrier
  0x00000048: OP_JOIN          1, 2  ; strategy=all, count=2
```

---

## 5. Security & Verification

1. **Signature Verification**:
   The host VM validates the `.nbc` file before parsing constants or instructions. It verifies the header's 64-byte `signature` field using the host-configured **Ed25519 public key**.
2. **Execution Gate**:
   If the signature check fails, NVM aborts immediately returning `ERR_VM_ILLEGAL_INSTRUCTION`.
3. **Execution Safety**:
   * **Stack Overflow**: Every push validates `ctx->sp < VM_STACK_LIMIT`.
   * **Jump Boundaries**: Jump offsets must remain within `ctx->code_size`.
