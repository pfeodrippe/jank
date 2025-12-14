# nREPL Stack Overflow Fix for Complex C++ Headers

## Problem

The nREPL server crashed when evaluating `cpp/raw` code that includes complex C++ headers like `flecs.h`. The crash manifested as:
- `EXC_BAD_ACCESS (code=2)` - stack overflow
- 689+ recursive frames in Clang's template instantiation (`TransformCompoundStmt` -> `SubstStmt` -> `InstantiateFunctionDefinition`)

## Root Cause

The nREPL server's worker thread was using `std::thread` which defaults to ~512KB stack size on macOS. This is insufficient for flecs.h's deep template instantiation during Clang's compilation.

## Solution

Modified `src/cpp/jank/nrepl_server/asio.cpp` to use `pthread` with a 16MB stack size instead of `std::thread`.

### Changes Made

1. Added `#include <pthread.h>`

2. Changed member variable from `std::thread io_thread_` to `pthread_t io_thread_{}`

3. Replaced thread creation code:
```cpp
// Before: std::thread with default ~512KB stack
io_thread_ = std::thread([this]() { ... });

// After: pthread with 16MB stack
pthread_attr_t attr;
pthread_attr_init(&attr);
constexpr size_t LARGE_STACK_SIZE = 16 * 1024 * 1024;
pthread_attr_setstacksize(&attr, LARGE_STACK_SIZE);

auto thread_func = [](void *arg) -> void * {
  auto *self = static_cast<server *>(arg);
  GC_stack_base sb;
  GC_get_stack_base(&sb);
  GC_register_my_thread(&sb);
  self->io_context_.run();
  GC_unregister_my_thread();
  return nullptr;
};

pthread_create(&io_thread_, &attr, thread_func, this);
pthread_attr_destroy(&attr);
```

4. Changed thread join from `io_thread_.join()` to `pthread_join(io_thread_, nullptr)`

## Additional Safety Mechanisms (Already Present)

The codebase also has signal recovery mechanisms in `include/cpp/jank/nrepl_server/ops/eval.hpp`:

- `sigaltstack()` with 128KB alternate stack for signal handlers
- `SA_ONSTACK` flag so signal handlers can run during stack overflow
- `setjmp/longjmp` recovery for SIGSEGV, SIGABRT, SIGBUS signals
- JIT recovery point for LLVM fatal errors

These provide graceful error recovery if stack overflow still occurs, but the primary fix is the larger thread stack size.

## Additional Fix: Idempotent load_object

### Problem
When CIDER sends `load-file`, it re-evaluates the entire file. If the file calls `load_object` (to load flecs.o for example), LLVM throws a "duplicate definition of symbol" error because it tries to load the same object file twice.

### Solution
Modified `src/cpp/jank/jit/processor.cpp` and `include/cpp/jank/jit/processor.hpp` to track loaded object files.

1. Added to processor.hpp:
```cpp
#include <unordered_set>
// ...
mutable std::unordered_set<std::string> loaded_objects_;
```

2. Modified `load_object` in processor.cpp:
```cpp
void processor::load_object(jtl::immutable_string_view const &path) const
{
  /* Canonicalize the path to detect duplicates even with different relative paths. */
  std::error_code ec;
  auto const canonical_path{ std::filesystem::canonical(std::string_view{ path }, ec) };
  std::string const path_key{ ec ? std::string{ path } : canonical_path.string() };

  /* Skip if already loaded - makes this idempotent for nREPL load-file operations. */
  if(loaded_objects_.contains(path_key))
  {
    return;
  }

  // ... original loading code ...

  loaded_objects_.insert(path_key);
  register_jit_stack_frames();
}
```

## Testing

After the fix:
- `(cpp/raw "#include \"flecs.h\"")` compiles successfully in nREPL
- Server remains running after processing complex headers
- CIDER `load-file` works without duplicate symbol errors
- All existing tests pass

## Key Learnings

1. **macOS thread stack sizes**: Non-main threads default to ~512KB (can verify with `ulimit -s`). Main thread gets 8MB.

2. **Clang template instantiation**: Heavy C++ template metaprogramming (like in flecs.h) can require deep recursion during compilation.

3. **Debugging vs runtime behavior**: `lldb` catches Mach exceptions before they become Unix signals, so `sigaltstack` recovery works at runtime but not under the debugger.

4. **pthread vs std::thread**: `std::thread` doesn't expose stack size configuration. Use `pthread_attr_setstacksize()` when you need control over thread stack size.

5. **LLVM addObjectFile is not idempotent**: Loading the same object file twice causes `llvm::cantFail` to abort with "duplicate definition of symbol". Track loaded objects and skip if already loaded.
