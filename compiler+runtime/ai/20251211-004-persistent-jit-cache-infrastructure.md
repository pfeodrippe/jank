# Persistent JIT Cache Infrastructure

## Date: 2025-12-11

## Summary
Created infrastructure for disk-based JIT caching. The goal is to save compiled defs to disk so they can be loaded in subsequent sessions, avoiding the slow C++ JIT compilation.

## What Was Implemented

### 1. Persistent Cache Class (`jit/persistent_cache.hpp/cpp`)
- `persistent_cache` class with methods:
  - `save_source()` - saves C++ source and metadata to disk
  - `load_entry()` - loads cached entry by hash
  - `has_cached_source()` - checks if cache entry exists
  - `get_stats()` - returns cache statistics
  - `clear()` - clears the cache

### 2. Cache Directory Structure
```
~/.cache/jank/<version>/jit_cache/
├── <hash>.cpp     # C++ source
├── <hash>.meta    # Metadata: qualified_name, unique_name
```

### 3. Thread-Local Context (`jit_cache_context`)
- Used to pass expression hash between `eval(expr::def_ref)` and `eval(expr::function_ref)`
- Fields: `current_hash`, `is_compiling`, `current_decl_str`, `current_unique_name`

### 4. CLI Flag
- `--no-persistent-cache` to disable the feature
- Default: enabled (but integration not active yet)

### 5. Context Integration
- Added `persistent_jit_cache` member to `runtime::context`
- Initialized with `binary_version` for cache isolation between versions

## What's Not Working Yet

The integration in `evaluate.cpp` was causing segmentation faults. The code to:
1. Set thread-local context before `eval(value_expr)`
2. Capture C++ source in `eval(expr::function_ref)`
3. Save to disk after compilation

...was removed because it caused crashes during the test suite.

## Root Cause of Crashes

Unclear, but likely related to:
- Thread-local storage access during complex evaluation
- String lifetime issues when passing between functions
- Interaction with JIT compilation

## Next Steps

1. **Investigate crash**: Debug why thread-local context access causes segfaults
2. **Alternative approach**: Instead of thread-local storage, consider:
   - Computing hash directly in `eval(expr::function_ref)`
   - Passing hash as a parameter through the eval chain
3. **Object file caching**: The real goal is to cache compiled `.o` files, not just C++ source:
   - Use LLVM's `ObjectLinkingLayer` plugin to intercept object files
   - Save them with expression hash as filename
   - Load them directly instead of JIT compiling

## Files Modified

### New Files:
- `include/cpp/jank/jit/persistent_cache.hpp`
- `src/cpp/jank/jit/persistent_cache.cpp`

### Modified Files:
- `CMakeLists.txt` - added persistent_cache.cpp
- `include/cpp/jank/util/cli.hpp` - added `persistent_jit_cache_enabled` flag
- `src/cpp/jank/util/cli.cpp` - added `--no-persistent-cache` argument
- `include/cpp/jank/runtime/context.hpp` - added `persistent_jit_cache` member
- `src/cpp/jank/runtime/context.cpp` - initialize `persistent_jit_cache`

## Usage (Future)

When the integration is complete:
- First session: ~4s (JIT compile, save to disk)
- Subsequent sessions: ~0.1-0.5s (load from disk)

Currently the persistent cache is:
- Infrastructure: Complete
- Integration: Not active (crashes)
- Performance benefit: None yet
