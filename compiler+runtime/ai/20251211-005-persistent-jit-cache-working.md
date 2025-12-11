# Persistent JIT Cache - Working Implementation

## Date: 2025-12-11

## Summary
Completed the persistent JIT cache infrastructure that saves C++ source to disk. Fixed the crash that was occurring when using thread-local storage for passing context between `eval(expr::def_ref)` and `eval(expr::function_ref)`.

## What Was Fixed

### 1. Thread-Local Context Crash
The original crash was due to the persistent cache code being inside the `if(util::cli::opts.jit_cache_enabled)` block. When testing with `jit_cache_enabled=false`, the persistent cache code was never reached, but when it was inside the block, there was a structural issue with how the code flowed.

**Solution**: Refactored `eval(expr::def_ref)` to:
1. Compute hash early if either cache is enabled
2. Check in-memory cache only if `jit_cache_enabled`
3. Set up thread-local context before eval if `persistent_jit_cache_enabled`
4. Compile the expression
5. Save to disk if `persistent_jit_cache_enabled`
6. Store in memory cache if `jit_cache_enabled`

### 2. Hex Formatting Bug
The filenames were being saved with literal `{:016x}` format string because `util::format()` only supports `{}` placeholders, not fmt-style format specifiers.

**Solution**: Created a `format_hash()` helper function using `std::ostringstream` with `std::hex`, `std::setw(16)`, `std::setfill('0')`, and `std::locale::classic()` to avoid thousand separators.

## Files Modified

### `src/cpp/jank/evaluate.cpp`
- Refactored `eval(expr::def_ref)` to support persistent cache independently of in-memory cache
- Added C++ source capture in `eval(expr::function_ref)`:
  ```cpp
  auto &cache_ctx = jit::get_jit_cache_context();
  if(cache_ctx.is_compiling && util::cli::opts.persistent_jit_cache_enabled)
  {
    cache_ctx.current_decl_str = cg_prc.declaration_str();
    cache_ctx.current_unique_name = expr->unique_name;
  }
  ```

### `src/cpp/jank/jit/persistent_cache.cpp`
- Added `#include <locale>` and `#include <iomanip>`
- Added `format_hash()` helper function
- Fixed path construction to use `format_hash(hash) + ".cpp"` instead of broken format

### `test/cpp/jank/perf/eval_benchmark.cpp`
- Added `#include <jank/jit/persistent_cache.hpp>`
- Added new test case: "Persistent JIT Cache: Infrastructure Test"

## Test Results

### In-Memory JIT Cache Test
```
| Scenario                | Time (ms) |
|-------------------------|-----------|
| Initial load            |      1733 |
| Reload WITH cache       |        15 |
| Reload WITHOUT cache    |      1744 |
|-------------------------|-----------|
| Speedup from cache      |   116.3x |
```

### Persistent JIT Cache Test
```
| Operation               | Time (ms) |
|-------------------------|-----------|
| Initial define          |       914 |
| Redefine (no cache)     |       876 |
|-------------------------|-----------|
| Disk cache entries      |         6 |
```

## Cache Directory Structure
```
~/.cache/jank/<version>/jit_cache/
├── 000a0f34e68a92a1.cpp     # C++ source
├── 000a0f34e68a92a1.meta    # Metadata: qualified_name\nunique_name
├── 000a0f3516c6ea90.cpp
├── 000a0f3516c6ea90.meta
...
```

## What's Working Now

1. **Persistent cache saves C++ source to disk** - Every `defn` saves its generated C++ code
2. **Proper hex filenames** - Files are named with 16-character zero-padded hex hashes
3. **Thread-local context works** - Context is properly passed between eval functions
4. **Both caches work independently** - Can enable/disable either cache separately
5. **Tests pass** - Both cache tests pass, full suite has same status as before

## What's NOT Yet Implemented

The actual **performance benefit** of persistent caching requires loading the cached content to skip JIT. Options:
1. **Re-compile saved C++** - Would still be slow (same JIT overhead)
2. **Cache object files** - The real goal (skip JIT entirely by loading pre-compiled `.o` files)

The current implementation is the **infrastructure** - saving works, but loading to skip JIT requires:
- Hooking into LLVM's `ObjectLinkingLayer` to intercept object files
- Saving them to disk with expression hash
- Loading them directly on cache hit

## CLI Flags
- `--no-jit-cache` - Disables in-memory JIT cache
- `--no-persistent-cache` - Disables disk-based persistent cache

Both are enabled by default.
