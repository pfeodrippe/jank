# nREPL NS Form Crash Investigation

## Problem Summary

When running the jank nREPL server from `/Users/pfeodrippe/dev/something` directory, evaluating `ns` forms via nREPL causes the connection to close immediately without any response. The server remains running but the specific connection is terminated.

**Working scenario**: Running from `/Users/pfeodrippe/dev/jank/compiler+runtime` directory
**Failing scenario**: Running from `/Users/pfeodrippe/dev/something` directory

## Command to Start Server

```bash
cd /Users/pfeodrippe/dev/something
PATH="/Users/pfeodrippe/dev/jank/compiler+runtime/build:$PATH" jank --module-path src run-main my-example start-server
```

## Test Results

### From `/dev/something` directory:

| Test | Result |
|------|--------|
| `(+ 1 2)` | ✅ Works |
| `(in-ns 'my-example)` | ✅ Works |
| `(in-ns 'brand-new-ns)` | ✅ Works |
| `(defn foo [] 42)` | ✅ Works |
| `(refer 'clojure.core)` | ✅ Works |
| `(ns test-ns-1)` | ❌ Connection closed |
| `(ns my-example (:require ...))` | ❌ Connection closed |

### From `compiler+runtime` directory:

| Test | Result |
|------|--------|
| All of the above | ✅ All work |

## Key Observations

1. **Not a crash**: The server stays running after the connection closes. This suggests the issue is in connection handling, not a segfault.

2. **Specific to `ns` macro**: Simple forms work. `in-ns` works. `refer` works. Only the `ns` macro fails.

3. **Directory-dependent**: Same code works from `compiler+runtime` but fails from `/dev/something`.

4. **Session namespace**: When clone is called, the new session inherits `current_ns` from the parent session (see `clone.hpp:13`).

5. **First operation after clone**: The failure happens on the FIRST `ns` form after cloning a session. Other operations work fine.

## Hypothesis

The issue might be related to:

1. **Module path resolution**: The `--module-path src` might affect how the `ns` macro resolves dependencies differently between directories.

2. **Exception escaping**: An uncaught exception in `process_buffer` -> `engine_.handle` -> `handle_eval` that bypasses all catch blocks and destroys the connection.

3. **Session state**: Something about the session's `current_ns` state when combined with the `ns` macro expansion.

## Code Path

1. Client sends bencode-encoded eval request with `ns` form
2. `connection::on_read` receives data
3. `connection::process_buffer` decodes bencode and calls `engine_.handle(msg)`
4. `engine::handle_eval` is called
5. `__rt_ctx->eval_string(code_view)` evaluates the `ns` macro
6. **Something happens here that causes the socket to close**
7. No response is sent to the client

## Files Involved

- `src/cpp/jank/nrepl_server/asio.cpp` - Connection handling
- `include/cpp/jank/nrepl_server/engine.hpp` - Engine and message routing
- `include/cpp/jank/nrepl_server/ops/eval.hpp` - Eval handler with exception handling
- `include/cpp/jank/nrepl_server/ops/clone.hpp` - Session cloning
- `src/jank/clojure/core.jank` - The `ns` macro definition

## Exception Handling in eval.hpp

The eval handler has these catch blocks:
- `catch(runtime::object_ref const &ex_obj)` - jank runtime exceptions
- `catch(jank::error_ref const &err)` - jank errors with source info
- `catch(std::exception const &ex)` - standard C++ exceptions
- `catch(...)` - catch-all for unknown exceptions

If an exception escapes all of these, the connection's `process_buffer` would terminate abnormally, causing the `shared_ptr<connection>` to be destroyed and the socket to close.

## Next Steps

1. Add debug output to `process_buffer` to see if an exception is being thrown
2. Check what happens during `ns` macro expansion that could throw an uncaught exception
3. Compare the runtime context state between the two directories
4. Check if the `ns` macro is trying to access files/modules that don't exist in `/dev/something`

## Reproduction Script

```python
import socket

def bencode_str(s):
    return f"{len(s)}:{s}"

def bencode_dict(d):
    result = "d"
    for k, v in sorted(d.items()):
        result += bencode_str(k)
        if isinstance(v, str):
            result += bencode_str(v)
    result += "e"
    return result

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', 5557))

# Clone session
s.sendall(bencode_dict({"op": "clone"}).encode())
print(s.recv(8192).decode())

# This will fail from /dev/something
s.sendall(bencode_dict({"op": "eval", "code": "(ns test-ns)"}).encode())
s.settimeout(5)
try:
    data = s.recv(8192)
    if not data:
        print("Connection closed!")
    else:
        print(data.decode())
except socket.timeout:
    print("Timeout")
```

---
Last updated: 2025-11-26
