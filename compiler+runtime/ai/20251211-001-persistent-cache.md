# Persistent JIT Cache Implementation

## Summary

Implemented persistent JIT cache infrastructure for jank. This allows C++ source code
generated during JIT compilation to be saved to disk, enabling future cross-session caching.

## What Was Implemented

### 1. Cache File Storage
- C++ source files (.cpp) - generated C++ code for function definitions
- Expression files (.expr) - expression strings for creating function instances
- Metadata files (.meta) - JSON metadata including symbol name and unique name
- Files are stored in `~/.cache/jank/<version>/jit_cache/<hash>.*`

### 2. Hash-Based Caching
- Uses `analyze::expression_hash` to compute deterministic hashes for function bodies
- Hash is based on the AST structure, not source positions
- Same function body = same hash = cache hit

### 3. Thread-Local Context
- Added `jit_cache_context` to pass context between `eval(expr::def_ref)` and `eval(expr::function_ref)`
- Captures C++ source, expression string, and unique name during compilation

### 4. Test Infrastructure
- Added "Persistent JIT Cache: Infrastructure Test" - tests saving to disk
- Added "Persistent JIT Cache: Cross-Session Simulation" - tests hash-based cache entries

## Key Files Modified

- `include/cpp/jank/jit/persistent_cache.hpp` - Header with cache context and methods
- `src/cpp/jank/jit/persistent_cache.cpp` - Implementation of save/load methods
- `src/cpp/jank/evaluate.cpp` - Integration into the eval pipeline
- `test/cpp/jank/perf/eval_benchmark.cpp` - Test cases

## What's Working

1. Cache files are saved correctly (.cpp, .expr, .meta)
2. Hash computation correctly identifies identical function bodies
3. Different function bodies create different cache entries
4. Cache entries can be loaded (via `load_entry`)

## What's TODO (Cross-Session Loading)

The cache loading was designed but disabled because:
- Within the same JIT session, reloading cached code causes struct redefinition errors
- The struct names conflict because they're already compiled in the current session

To enable cross-session speedup:
1. The loading should only be attempted when starting a fresh jank process
2. Need to track whether structs have been compiled in the current session
3. Or use a sub-interpreter approach for cache loading

## Usage

```bash
# Enable persistent cache (default: on)
./build/jank --persistent-jit-cache ...

# Disable persistent cache
./build/jank --no-persistent-jit-cache ...
```

## Test Results

```
./build/jank-test --test-case='*Persistent*'
[doctest] test cases:  2 |  2 passed | 0 failed
[doctest] Status: SUCCESS!
```

## Cache Directory Structure

```
~/.cache/jank/<version>/jit_cache/
├── 000a0f34e68a92a1.cpp   # C++ source
├── 000a0f34e68a92a1.expr  # Expression string
├── 000a0f34e68a92a1.meta  # Metadata JSON
├── 000a0f35e92f1234.cpp
├── 000a0f35e92f1234.expr
├── 000a0f35e92f1234.meta
└── ...
```
