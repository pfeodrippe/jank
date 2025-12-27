# Boehm GC Thread Registration

## Overview

jank uses the Boehm-Demers-Weiser garbage collector (bdwgc) for automatic memory management. When using multiple threads that allocate GC-managed memory, each thread must be registered with the GC.

## The Problem

The Boehm GC needs to scan all thread stacks to find root pointers during garbage collection. If a thread allocates GC memory without being registered, the GC cannot safely scan that thread's stack. On macOS (Darwin), this causes `GC_push_all_stacks` in `darwin_stop_world.c` to call `abort()`, crashing the process.

### Symptoms

- `zsh: abort` when running jank with nREPL server
- SIGABRT in lldb with stack trace showing:
  ```
  GC_push_all_stacks at darwin_stop_world.c
  GC_default_push_other_roots
  GC_push_roots
  GC_mark_some
  GC_stopped_mark
  GC_try_to_collect_inner
  ```

## The Solution

Any thread that will allocate GC memory must register itself at startup and unregister before exit.

**Important:**
1. You must define `GC_THREADS` before including `gc/gc.h` to enable the thread registration functions
2. You must call `GC_allow_register_threads()` from the main thread before creating threads that will register themselves

```cpp
#define GC_THREADS
#include <gc/gc.h>

// In the main thread, before creating worker threads:
GC_allow_register_threads();

std::thread my_thread([this]() {
    // Register this thread with the GC
    GC_stack_base sb;
    GC_get_stack_base(&sb);
    GC_register_my_thread(&sb);

    // ... do work that allocates GC memory ...

    // Unregister before thread exit
    GC_unregister_my_thread();
});
```

Without `GC_THREADS`, the thread registration functions are not declared.

Without `GC_allow_register_threads()`, calling `GC_register_my_thread()` will abort with: "Threads explicit registering is not previously enabled".

## Affected Components

### nREPL Server (`src/cpp/jank/nrepl_server/asio.cpp`)

The nREPL server runs in a separate thread to handle client connections. When evaluating code via nREPL, the eval handler allocates GC memory (e.g., creating immer data structures). The server thread must be registered with the GC.

## Debugging Tips

1. Run with lldb to catch the abort:
   ```bash
   lldb -- jank --module-path src run-main my-example start-server
   ```

2. When it crashes, use `bt` to get the stack trace

3. Look for `GC_push_all_stacks` in the trace - this indicates an unregistered thread issue

## References

- [Boehm GC documentation on threads](https://github.com/ivmai/bdwgc/blob/master/doc/README.md)
- `GC_register_my_thread()` - Register calling thread with the collector
- `GC_unregister_my_thread()` - Unregister before thread termination
- `GC_get_stack_base()` - Get stack base for registration
