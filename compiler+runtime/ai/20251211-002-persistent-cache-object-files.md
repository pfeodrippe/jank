# Persistent JIT Cache: Object File Compilation

## Summary

Implemented full cross-session persistent JIT cache that compiles C++ source to object files (.o)
and automatically loads them on subsequent evaluations. This provides **~42-44x speedup** through actual
jank evaluation (verified through benchmarks using `eval_string()`).

## What Was Implemented

### 1. Fixed `compile_to_object()`

The implementation uses the same infrastructure as the JIT processor:

- Uses `JANK_JIT_FLAGS` - the compiled-in flags from AOT compilation
- Includes the PCH (precompiled header) via `-include-pch`
- Uses `invoke_clang()` for proper error handling
- Adds correct include paths for clang, jank resources
- Uses `-c -fPIC` for position-independent object code
- Suppresses stderr to avoid spam from expected failures (code using external types)

**Key changes to `persistent_cache.cpp`:**
- Added `binary_version_` member to find PCH
- Rewrote `compile_to_object()` to build proper args vector
- Factory function: `extern "C" jank_pcache_factory_<hash>()`
- Added stderr redirection using `dup2()` for silent compilation failures

### 2. Implemented Object File Loading in `eval(expr::def_ref)`

Cross-session loading happens automatically during normal jank evaluation:

```cpp
/* Persistent JIT: Check for pre-compiled .o file from previous session.
 * This is the fast path - skip parse/analyze/codegen/JIT entirely. */
if(use_persistent_cache)
{
  auto const &pcache = __rt_ctx->persistent_jit_cache;
  if(pcache.has_compiled_object(body_hash))
  {
    auto const obj_path = pcache.object_path(body_hash);
    auto const factory_name = jit::persistent_cache::factory_name(body_hash);

    try
    {
      /* Load the object file into the JIT. */
      __rt_ctx->jit_prc.load_object(obj_path.string());

      /* Find the factory function. */
      auto const sym_result = __rt_ctx->jit_prc.find_symbol(factory_name);
      if(sym_result.is_ok())
      {
        using factory_fn = object_ref (*)();
        auto const factory = reinterpret_cast<factory_fn>(sym_result.expect_ok());
        auto const evaluated_value = factory();
        var->bind_root(evaluated_value);

        /* Store in memory cache for future reuse within this session. */
        if(use_jit_cache)
        {
          __rt_ctx->jit_cache.store(qualified_name, body_hash, var);
        }

        pcache.record_disk_hit();
        return var;
      }
    }
    catch(...)
    {
      /* If loading failed, fall through to normal compilation path. */
    }
  }
}
```

### 3. Performance Test via Actual Jank Evaluation

The benchmark test measures real cross-session performance through jank's eval:

```
=== Results ===
| Scenario                | Time (us)   | Time (ms) |
|-------------------------|-------------|-----------|
| First eval (full JIT)   |      301031 |       301 |
| Cross-session (.o load) |        6805 |         6 |
|-------------------------|-------------|-----------|
| Time saved              |      294226 |     97.7% |
| Speedup                 |       44x |           |

✓ Object file cache is working!
  - First eval: 301 ms (full JIT pipeline)
  - Cross-session: 6805 us (load .o + call factory)
  - Speedup: 44x
```

## Cache Structure

```
~/.cache/jank/<version>/jit_cache/
├── <hash>.cpp     # C++ source
├── <hash>.expr    # Expression string for factory
├── <hash>.meta    # Metadata (qualified_name, unique_name)
└── <hash>.o       # Pre-compiled object file (~694 KB)
```

## How It Works

### First Session (Cold)
1. Parse + Analyze + Codegen + JIT compile + Execute
2. Save C++ source to `.cpp` file
3. Save expression to `.expr` file
4. Save metadata to `.meta` file
5. **Compile to .o file** (synchronously, with stderr suppressed)

### Automatic Object Compilation

Object compilation happens automatically after each def:
- Uses `invoke_clang()` with PCH for proper type definitions
- Errors are **suppressed** (stderr redirected to /dev/null) because:
  - Code using external types (`cpp/raw`, loaded headers) will fail
  - These failures are expected and shouldn't spam the user
  - Such defs simply won't have cached .o files and will JIT compile each session
- Only standard jank code (no external types) produces valid .o files

### Subsequent Sessions (Warm)
1. Check if `.o` file exists for hash
2. Load object file: `jit_prc.load_object(path)`
3. Find factory: `jit_prc.find_symbol("jank_pcache_factory_<hash>")`
4. Call factory to get the jank object
5. Skip parse/analyze/codegen/JIT entirely!

## Factory Function

The factory function is generated as:

```cpp
extern "C" jank::runtime::object* jank_pcache_factory_<hash>() {
  return <expression_string>;
}
```

This returns the jank function object directly, bypassing all compilation.

## Testing Cross-Session Loading

The benchmark test simulates cross-session behavior:
1. Evaluate code first time (full JIT, creates .o file)
2. Clear in-memory JIT cache
3. Re-evaluate the same code (loads from .o file)
4. Compare times

Real cross-session testing:
1. Run jank once to populate cache
2. Exit and start fresh jank process
3. The cached `.o` files will be loaded instead of JIT compiling

## Key Files Modified

- `include/cpp/jank/jit/persistent_cache.hpp` - Added `binary_version_` member
- `src/cpp/jank/jit/persistent_cache.cpp` - Fixed `compile_to_object()` to use invoke_clang with PCH, added stderr suppression
- `src/cpp/jank/evaluate.cpp` - Added object loading in `eval(expr::def_ref)`, calls `compile_to_object()` automatically
- `test/cpp/jank/perf/eval_benchmark.cpp` - Updated test to measure real speedup via jank eval

## Performance Results

| Scenario | Time | Notes |
|----------|------|-------|
| First eval (full) | 301 ms | Parse + Analyze + Codegen + JIT |
| Cross-session load | 6.8 ms | Load .o + call factory via jank eval |
| **Speedup** | **44x** | Skips 97.7% of time |

## Future Improvements

1. **Async object compilation**: Currently synchronous, could compile in background thread
   - Avoids blocking first eval with compile time
   - Needs careful handling of detached threads and process lifetime
2. **External type support**: Currently fails silently for code using `cpp/raw`, `cpp/include`
   - Could store loaded header paths in cache and include them during compilation
   - Would enable caching more code
3. **Cache eviction**: Implement LRU or size-based eviction
4. **Cache warming**: Pre-compile commonly used functions on install
5. **Partial loading**: Load only needed symbols from object files
