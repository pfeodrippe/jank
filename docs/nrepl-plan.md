# nREPL Bring-up Plan

## Context
- `jank` exposes a CLI `--server` flag and an `nrepl-server` subproject, but both are placeholders.
- The native `asio` bridge currently echoes bytes and never calls back into the runtime, so no nREPL ops are implemented.
- We need a functional nREPL endpoint that real clients (e.g. `lein repl :connect`, `cider-connect-clj`) can talk to.

## Objectives
1. Bootstrap the runtime exactly as the interactive REPL does (load `clojure.core`, enter `user`, refer `clojure.core`).
2. Listen on a configurable TCP port, accept multiple concurrent clients, and speak the nREPL bencode protocol.
3. Implement the core ops (`clone`, `describe`, `ls-sessions`, `eval`, `close`) with per-session namespace tracking and useful errors.
4. Surface evaluation results via `:value`/`:ns` responses, propagate exceptions, and always terminate requests with `:status ["done" …]`.
5. Drop an `.nrepl-port` marker for tooling and clean it up on shutdown.

## Implementation Plan
1. **Runtime bootstrap**: extend `jank.nrepl-server.asio/run!` so it loads `clojure.core`, switches to the `user` ns, and refers `clojure.core` before entering the IO loop.
2. **Protocol engine**: embed a small bencode codec and nREPL dispatcher in C++. Maintain session state (current ns) per client and gate all evaluations through `context::binding_scope` to isolate `*ns*`/`*1..*3`.
3. **Operations**:
   - `clone`: generate UUIDs via `runtime::random_uuid`, cache a new session, return `:new-session`.
   - `describe`: announce supported ops and runtime version (via `clojure.core/jank-version`).
   - `ls-sessions`: return the sorted session id list.
   - `eval`: call `__rt_ctx->eval_string` under the session bindings, stream back `:value` then `:done` (and `:err` on failure).
   - `close`: drop the session and acknowledge.
   - Default: reply with `:status ["unsupported" "done"]`.
4. **Transport glue**: replace the echo server with a framed TCP server that buffers reads, decodes complete messages, hands them to the dispatcher, encodes responses, and drains a write queue to preserve ordering.
5. **Entry point**: update `core.jank/-main` to accept an optional port argument, call `run!`, and log where the server is listening. Remove the unused placeholder namespaces.

## Verification
1. `cd nrepl-server && lein jank run 5555` (or build via `./compiler+runtime/bin/jank repl --server`).
2. In another terminal, `nc localhost 5555` or `clj -M:nrepl -m nrepl.cmdline --connect --host localhost --port 5555`.
3. Send `clone` + `eval` requests; expect `:value` and `:status ["done"]` responses and namespace tracking.
4. Run `ls-sessions` and `close` to ensure lifecycle works.

## Risks / Follow-ups
- We are not yet streaming `*out*`/`*err*`; that will require a custom writer binding middleware.
- Interrupts and `stdin` ops are not implemented.
- No TLS/auth; front with SSH tunnels for now.
