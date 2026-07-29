# Nestor Orchestration Engine

High-performance, memory-optimized workflow orchestration engine written in C.

## Language

**Workflow**:
A directed acyclic graph (DAG) of **Jobs** defining the execution flow.
_Avoid_: Pipeline, process, graph

**Job**:
An execution node in the **Workflow** containing a sequential list of **Steps** that executes concurrently with other independent **Jobs**.
_Avoid_: Task, block, group

**Step**:
An individual execution block within a **Job**, either performing an asynchronous HTTP call or launching a subprocess plugin.
_Avoid_: Action, command, instruction

**PluginExecutor**:
A deep module managing subprocess plugin execution, non-blocking standard I/O pipes, child process signals, and the interactive Unix socket IPC server.
_Avoid_: SubprocessRunner, PluginHandler

**Transport**:
An abstract seam decoupling HTTP execution from the underlying networking library (libcurl), allowing production and mock in-memory adapters.
_Avoid_: HTTPClient, WebClient

**Evaluator**:
A deep module consolidating expression evaluation (JSONata), template interpolation (`{{...}}`), and duration parsing.
_Avoid_: Parser, Interpolator

**Transform**:
A native, zero-copy, in-process job type executing JSONata transformations on the workflow context.
_Avoid_: MapJob, ConvertNode

**ConditionalDependency**:
An edge-based execution logic constraint specifying satisfying terminal states (onSuccess, onFailure, onCompletion, onSkip) for upstream execution.
_Avoid_: TriggerRule, NodeCondition

**DynamicPlugin**:
A dynamic shared library (.so/.dylib) loaded in-process at runtime, sharing the host engine's memory arenas.
_Avoid_: SharedLibraryAddon, NativeExtension

**SubprocessSandboxing**:
A mechanism executing untrusted plugins in an isolated helper subprocess wrapper to isolate execution failures.
_Avoid_: PluginContainer, IsolationHost

**SQLiteCache**:
A local caching subsystem employing TTL-based expiration and LRU size-capping eviction strategies.
_Avoid_: LocalStoreCache, StateCache

**DeterministicBoundary**:
A graph validation rule forcing workflows to have exactly one start job (entry point) and joining all parallel forks before hitting any exit/return jobs.
_Avoid_: SingleEntryPointRule, JoinedValidation

**ReturnJob**:
A dedicated job type that marks the explicit exit point of a workflow path, evaluating a deterministic outcome value.
_Avoid_: ExitJob, EndNode

**VariableVisibility**:
The scoping boundary of a job variable, either private (internal to step execution and omitted from serialized storage) or public (exported to downstream jobs).
_Avoid_: VariableScope, LocalExport

**StepProjection**:
A step-level output mapping that filters response payloads and releases raw buffer data from memory immediately on completion.
_Avoid_: BodyFilter, ResponseTruncator

**ProviderConfiguration**:
A root-level declaration initializing shared session handles (connection pools, sockets, authentication headers) for execution steps.
_Avoid_: SessionPool, ConnectionConfig

**StateBackend**:
A state serialization subsystem saving active stack frames, queues, and registers to `.nestor.tfstate` for workflow suspension and locking.
_Avoid_: ContextPersister, StateLogger

**DeclarativeProvider**:
A YAML provider contract mapping operations directly to Nestor's native HTTP, SQL, or plugin runners without C compilation.
_Avoid_: NoCodePlugin, WrapperProvider

## Example Dialogue

**Developer**: How do we prevent **Steps** inside a **Job** from polluting each other's output?
**Domain Expert**: Each **Job** execution tracks its outcomes in a structured context, but when a **Job** runs a loop, its local sub-arena is recycled on each iteration.
**Developer**: And if a **Step** needs to query other **Job** variables at runtime?
**Domain Expert**: The **PluginExecutor** exposes a Unix socket where the plugin can query the **Evaluator** to resolve paths in the active context dynamically.

