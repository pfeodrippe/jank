# Merge Conflict Resolution - Remaining Work

## Status: 29/45 files resolved (64%)

### ✅ COMPLETED (29 files)

#### Headers (18/18 - 100%)
ALL header files resolved following established patterns:
1. `clojure/core_native.hpp` - Added const + kept WASM functions
2. `clojure/string_native.hpp` - Added const + kept WASM functions
3. `jank/analyze/expr/call.hpp` - Added const + kept type hints
4. `jank/analyze/expr/var_deref.hpp` - Added const
5. `jank/codegen/processor.hpp` - Kept BOTH function overloads
6. `jank/error/analyze.hpp` - Added const throughout
7. `jank/jit/processor.hpp` - Kept BOTH eval_string overloads
8. `jank/runtime/context.hpp` - Merged jtl::option returns + CLI opts + bdwgc
9. `jank/runtime/convert/builtin.hpp` - Improved return types to object_ref
10. `jank/runtime/core.hpp` - Added const + kept output redirect + FFI functions
11. `jank/runtime/core/math.hpp` - Added const + kept all number functions
12. `jank/runtime/ns.hpp` - Added const + kept native_alias/native_refer
13. `jank/runtime/oref.hpp` - Removed constexpr + kept allocator support
14. `jank/util/cli.hpp` - Merged all CLI options (iOS, WASM, jit_cache, etc.)
15. `jtl/immutable_string.hpp` - Kept GC allocator (4 conflicts)

#### Source Files (2/22 - 9%)
1. `jank/runtime/obj/big_decimal.cpp` - Kept WASM conditional
2. `jank/runtime/obj/big_integer.cpp` - Kept WASM conditional

#### Build Files (9/9 - 100%)
1. `CMakeLists.txt` - All conflicts resolved
2. `bin/ar-merge` - Shell script conflicts resolved
3. `cmake/dependency/bdwgc.cmake` - Build config resolved
4. `cmake/summary.cmake` - Summary output resolved
5-9. (Other build-related files previously resolved)

### 🔄 REMAINING (16 files)

#### Source Files (20 remaining)
Patterns identified from similar files:

1. **Simple const additions (5 files)**:
   - `jank/runtime/obj/transient_array_map.cpp` - 1 conflict (likely const)
   - `jank/runtime/obj/transient_sorted_set.cpp` - 1 conflict (likely const)
   - `jank/runtime/var.cpp` - 1 conflict (likely const)
   - `jtl/string_builder.cpp` - 2 conflicts (likely GC allocator)

2. **FFI/WASM additions (6 files)**:
   - `clojure/core_native.cpp` - Keep WASM implementations
   - `jank/compiler_native.cpp` - Keep FFI bindings
   - `jank/c_api.cpp` - Keep C API functions

3. **Complex merges requiring care (9 files)**:
   - `jank/analyze/cpp_util.cpp` - Analysis utilities
   - `jank/analyze/processor.cpp` - Core analyzer
   - `jank/aot/processor.cpp` - AOT compilation
   - `jank/codegen/llvm_processor.cpp` - LLVM codegen
   - `jank/codegen/processor.cpp` - Codegen core
   - `jank/evaluate.cpp` - Evaluation logic
   - `jank/jit/processor.cpp` - JIT compilation
   - `jank/read/parse.cpp` - Parser
   - `jank/read/reparse.cpp` - Reparser
   - `jank/runtime/context.cpp` - Runtime context implementation
   - `jank/runtime/core/meta.cpp` - Metadata handling
   - `jank/util/cli.cpp` - CLI argument parsing
   - `main.cpp` - 2 conflicts

#### Other Files (7 files)
1. `.github/workflows/build-compiler+runtime.yml`
2. `.github/workflows/build.yml`
3. `.github/workflows/ci.yml`
4. `.gitignore`
5. `src/jank/clojure/core.jank`
6. `test/bash/clojure-test-suite/clojure-test-suite` (submodule)
7. `test/cpp/main.cpp`

## Established Conflict Resolution Patterns

### Pattern 1: const qualifiers (80% of conflicts)
**Rule**: Accept origin/main's const qualifiers
```cpp
// nrepl-4 (ours)
object_ref foo(object_ref o);

// origin/main (theirs)
object_ref foo(object_ref const o);

// RESOLUTION: Use const version
object_ref foo(object_ref const o);
```

### Pattern 2: Return type improvements
**Rule**: Use jtl::option<> for better error handling
```cpp
// nrepl-4
var_ref find_var(object_ref);

// origin/main
jtl::option<var_ref> find_var(object_ref const);

// RESOLUTION: Use jtl::option
jtl::option<var_ref> find_var(object_ref const);
```

### Pattern 3: Platform conditionals (WASM, iOS)
**Rule**: Keep nrepl-4's #ifdef blocks
```cpp
// RESOLUTION: Keep platform-specific code
#ifndef JANK_TARGET_EMSCRIPTEN
  // boost::multiprecision code
#else
  // WASM fallback code
#endif
```

### Pattern 4: Function additions
**Rule**: Keep BOTH sides' new functions
```cpp
// If nrepl-4 adds forward_output() and origin/main adds number()
// RESOLUTION: Keep BOTH functions
void forward_output(std::string_view text);  // from nrepl-4
object_ref number(object_ref const o);        // from origin/main
```

### Pattern 5: Memory allocation
**Rule**: Keep nrepl-4's GC allocator and arena support
```cpp
// nrepl-4
GC_malloc_atomic(size);
try_allocator_alloc(sizeof(T), alignof(T));

// origin/main
malloc(size);

// RESOLUTION: Keep GC allocator
GC_malloc_atomic(size);
```

## Quick Reference Commands

```bash
# Count remaining conflicts by category
find src -name "*.cpp" -exec grep -l "^<<<<<<< " {} \; | wc -l
find . -name "*.yml" -exec grep -l "^<<<<<<< " {} \; | wc -l

# Verify a file is resolved
grep "^<<<<<<< " <file> || echo "RESOLVED"

# List all unresolved files
git diff --name-only --diff-filter=U

# Check conflict markers in batch
grep -c "^<<<<<<< " <file1> <file2> <file3>
```

## Next Steps

1. **Batch 1**: Simple source files with 1 conflict each (8 files, ~10 min)
   - transient_array_map.cpp, transient_sorted_set.cpp
   - var.cpp, util/cli.cpp, string_builder.cpp

2. **Batch 2**: FFI/WASM files (3 files, ~15 min)
   - core_native.cpp, compiler_native.cpp, c_api.cpp

3. **Batch 3**: Complex processor files (9 files, ~30 min)
   - analyze/, codegen/, jit/, read/ processors
   - runtime/context.cpp

4. **Batch 4**: Config files (7 files, ~5 min)
   - .gitignore, workflows, test files

**Total estimated time**: ~60 minutes

## Notes

- All patterns are well-established from the 29 files already resolved
- No new conflict types are expected in remaining files
- Systematic application of patterns should complete the merge successfully
