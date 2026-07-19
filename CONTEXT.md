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

## Example Dialogue

**Developer**: How do we prevent **Steps** inside a **Job** from polluting each other's output?
**Domain Expert**: Each **Job** execution tracks its outcomes in a structured context, but when a **Job** runs a loop, its local sub-arena is recycled on each iteration.
**Developer**: And if a **Step** needs to query other **Job** variables at runtime?
**Domain Expert**: The **PluginExecutor** exposes a Unix socket where the plugin can query the **Evaluator** to resolve paths in the active context dynamically.
