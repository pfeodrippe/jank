# nREPL Thread Bindings Fix

## Problem
After starting the nREPL server on iOS, crash with:
```
[jank] Error calling -main: Cannot set non-thread-bound var: #'clojure.core/*ns*
```

## Root Cause
The nREPL server creates an IO thread (via `pthread_create`) to handle async operations. When eval is called from a connected client, the `(in-ns ...)` operation tries to set `*ns*`, but the IO thread doesn't have thread-local bindings set up.

## Solution
Added `binding_scope` to the nREPL server's IO thread startup in `asio.cpp`:

```cpp
auto thread_func = [](void *arg) -> void * {
  auto *self = static_cast<server *>(arg);
  /* Set up thread bindings for dynamic vars like *ns*.
   * This is required because eval operations may call (in-ns ...) which
   * needs *ns* to be thread-bound. */
  jank::runtime::context::binding_scope bindings;
  self->io_context_.run();
  return nullptr;
};
```

## Files Changed
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/nrepl_server/asio.cpp`: Added binding_scope in IO thread

## Rebuild Steps
1. Rebuild jank: `ninja -C build jank`
2. Rebuild iOS device JIT: `./bin/build-ios build-ios-device-jit Debug device jit`
3. Copy libraries: `make ios-jit-device-libs`
4. Rebuild from Xcode
