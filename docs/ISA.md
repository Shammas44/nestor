# Nestor VM Instruction Set Architecture (ISA)

---

## 1. Virtual Machine Execution Model

Nestor VM (NVM) is a stack-based virtual machine. It executes bytecode instructions sequentially or concurrently by operating on two primary stack structures and a data constant pool.

### 1.1 Virtual Machine State
The NVM execution context maintains the following register and state structures:
*   **PC (Program Counter)**: Pointer offset targeting the active instruction byte within the code segment.
*   **SP (Stack Pointer)**: Index targeting the top of the evaluation stack.
*   **Evaluation Stack**: A fixed-size array of `Jsonv_Value` structures used to pass arguments to operations and hold intermediate expressions.
*   **Call Stack**: A linked stack of `NVMCallFrame` records tracking hierarchical sub-workflow returns.
*   **Constant Pool**: A read-only lookup table of static strings and scalar values loaded from the bytecode header.

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
│ Table of Offsets: Const Pool Offset | Code Segment Offset   │
├─────────────────────────────────────────────────────────────┤
│ Constant Pool: Length-prepended static string data          │
├─────────────────────────────────────────────────────────────┤
│ Instruction Segment: Opcode (1B) + Operands (var length)    │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 The Header Struct
```c
typedef struct {
  char magic[4];             // Must be "NEST"
  uint16_t version;          // Target Nestor version (e.g. 3)
  uint16_t flags;            // Flags (e.g. signing enabled)
  uint32_t const_pool_offset;// Offset to Constant Pool
  uint32_t const_pool_count; // Number of entries in Constant Pool
  uint32_t code_offset;      // Offset to Instruction Segment
  uint32_t code_size;        // Size in bytes of Instruction Segment
} NVMHeader;
```

---

## 3. Instruction Set & Opcode Reference

All instructions consist of a 1-byte **Opcode** followed by zero or more operands. Operands are represented as standard network-byte-order integers (big-endian).

### 3.1 Operations Reference Table

| Opcode | Mnemonic | Operands | Description |
| :--- | :--- | :--- | :--- |
| `0x00` | `OP_NOP` | None | No operation. |
| `0x01` | `OP_PUSH_CONST` | `uint32_t const_idx` | Reads a value from the Constant Pool at `const_idx` and pushes it onto the evaluation stack. |
| `0x02` | `OP_POP` | None | Pops and discards the top value off the evaluation stack. |
| `0x03` | `OP_RESOLVE` | `uint32_t path_const_idx` | Reads a JSONata path string from Constant Pool, evaluates it against the active context, and pushes the result. |
| `0x04` | `OP_JUMP` | `uint32_t offset` | Sets the Program Counter (PC) directly to the target `offset`. |
| `0x05` | `OP_JUMP_IF_FALSE` | `uint32_t offset` | Pops the top evaluation stack value. If it is falsy, sets PC to `offset`. |
| `0x06` | `OP_CALL_PROVIDER` | `uint32_t prov_op_idx` | Invokes the dynamic provider operation referenced at constant index `prov_op_idx` using arguments popped from stack. |
| `0x07` | `OP_CALL_WORKFLOW` | `uint32_t wf_idx` | Instantiates a new call frame, pushes it onto Call Stack, isolates variables context, and jumps to workflow entrypoint. |
| `0x08` | `OP_RETURN` | None | Pops the active Call Stack frame and restores execution variables and PC to the parent frame. |
| `0x09` | `OP_FORK` | `uint8_t count`, `uint32_t offsets[]` | Enqueues multiple concurrent Program Counters into the NVM scheduling queue. |
| `0x0A` | `OP_JOIN` | `uint8_t join_strategy` | Synchronization barrier. Suspends execution path until fork paths complete matching `join_strategy`. |

---

## 4. Execution Cycle & Safety Limits

The NVM decode-execute loop runs within a single thread or multiplexes multiple concurrent task threads:

```c
int32_t nvm_execute_loop(NVMContext *ctx) {
  /*#region*/
  while (ctx->pc < ctx->code_size) {
    uint8_t opcode = ctx->code_segment[ctx->pc];
    ctx->pc++;

    switch (opcode) {
      case OP_NOP:
        break;
      case OP_PUSH_CONST: {
        uint32_t idx = read_uint32_be(ctx);
        if (idx >= ctx->const_pool_count) return ERR_VM_OUT_OF_BOUNDS;
        if (ctx->sp >= VM_STACK_LIMIT) return ERR_VM_STACK_OVERFLOW;
        ctx->stack[ctx->sp++] = get_constant(ctx, idx);
        break;
      }
      case OP_POP: {
        if (ctx->sp == 0) return ERR_VM_STACK_UNDERFLOW;
        ctx->sp--;
        break;
      }
      case OP_JUMP: {
        uint32_t offset = read_uint32_be(ctx);
        if (offset >= ctx->code_size) return ERR_VM_INVALID_JUMP;
        ctx->pc = offset;
        break;
      }
      // Other opcode cases...
      default:
        return ERR_VM_ILLEGAL_INSTRUCTION;
    }
  }
  return ERR_SUCCESS;
  /*#endregion*/
}
```

### 4.1 Safety Check Enforcement
*   **Stack Bounds**: Any push check `ctx->sp >= VM_STACK_LIMIT` prevents memory write-out-of-bounds corruption.
*   **Jump Bounds**: Branch offsets are checked against `ctx->code_size` before applying to prevent instruction pointer escapes.
*   **Constant Bounds**: String pointer accesses are validated against the constant table bounds.
