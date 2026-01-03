# Incremental JIT Cache Implementation

## Date: 2025-12-11

## Summary
Implemented Strategy 0: Incremental JIT - caching compiled defs to skip redundant JIT compilation when the same code is evaluated multiple times.

## Problem
When reloading a file or namespace in the REPL, ALL defs were being recompiled even if they hadn't changed. This made the development cycle slow.

## Solution
Added an incremental JIT cache that:
1. Hashes the expression body of each def by its **structure** (not source positions)
2. Stores `{qualified_name -> {hash, var_ref}}` in a cache
3. On re-evaluation of a def, checks if the hash matches a cached entry
4. If cache hit: returns the cached var (skips JIT compilation)
5. If cache miss: compiles normally and stores in cache

## Performance Results
```
| Scenario                | Time (ms) |
|-------------------------|-----------|
| Initial load (10 fns)   |       513 |
| Reload WITH cache       |        14 |
| Reload WITHOUT cache    |       391 |
| Speedup from cache      |    27.9x |
```

## Files Created/Modified

### New Files:
- `include/cpp/jank/analyze/expression_hash.hpp` - Header for expression hashing
- `src/cpp/jank/analyze/expression_hash.cpp` - Implementation of hash_expression() for all expression types
- `include/cpp/jank/jit/incremental_cache.hpp` - Cache data structures and methods

### Modified Files:
- `include/cpp/jank/util/cli.hpp` - Added `jit_cache_enabled` option (default: true)
- `src/cpp/jank/util/cli.cpp` - Added `--no-jit-cache` flag
- `include/cpp/jank/runtime/context.hpp` - Added `jit_cache` member to context
- `src/cpp/jank/evaluate.cpp` - Modified `eval(expr::def_ref)` to use cache
- `CMakeLists.txt` - Added expression_hash.cpp to build

## Usage
Cache is enabled by default. To disable:
```bash
jank --no-jit-cache <file>
```

## Key Design Decisions

1. **Hash by structure, not source positions**: The hash includes the expression kind, parameter names, body structure, and values but NOT line/column info, so the same code at different positions produces the same hash.

2. **Store var_ref in cache**: On cache hit, we return the cached var directly. The var's value is already bound from the first compilation.

3. **Cache per qualified symbol**: Keys are `namespace/name` symbols, so different namespaces can have functions with the same name without collision.

## Future Improvements
- Consider invalidation strategies for dependent defs
- Explore caching at the function arity level (not just whole defs)
- Add persistent disk cache for cross-session benefits
