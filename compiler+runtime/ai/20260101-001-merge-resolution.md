# Merge Resolution: origin/main into nrepl-4

**Date:** 2026-01-01
**Branch:** nrepl-4
**Merged from:** origin/main

## Summary

Successfully resolved all 45 merge conflicts when merging origin/main into the nrepl-4 branch. The merge strategy preserved iOS/WASM/nREPL functionality from nrepl-4 while incorporating new features from origin/main.

## Merge Strategy

### General Approach
1. For files with iOS/WASM/nREPL-specific functionality: Preferred nrepl-4 version
2. For new features in origin/main: Manually integrated into nrepl-4 codebase
3. For formatting-only conflicts: Preferred nrepl-4 version (already formatted)

### Key Conflicts Resolved

#### Build Configuration Files
- **CMakeLists.txt**: Manually merged
  - Added new options from origin/main: `jank_profile_gc`, `jank_force_phase_2`
  - Added `jank_enable_phase_2` conditional logic
  - Added phase 2 output variables (`jank_clojure_core_output`, `jank_clojure_core_output_artifact`)
  - Preserved iOS/WASM options from nrepl-4
  - Preserved nREPL core modules configuration from nrepl-4

- **cmake/dependency/bdwgc.cmake**: Manually merged
  - Preserved iOS/WASM conditional compilation flags
  - Added `jank_profile_gc` → `enable_valgrind_tracking` logic from origin/main
  - Kept iOS/WASM thread-local allocation and parallel marking optimizations
  - Added all new unset() calls for cleanup

- **cmake/summary.cmake**: Manually merged
  - Added `jank_phase_2` and `jank_profile_gc` to build summary output
  - Aligned formatting with origin/main

#### Workflow Files
- **.github/workflows/build.yml**: Removed (replaced by build-compiler+runtime.yml)

#### Header Files (17 files)
All header files preserved nrepl-4 version to maintain iOS/WASM/nREPL functionality:
- `include/cpp/jank/util/try.hpp`: Manually merged to fix `error_ref` const reference
- `include/cpp/jtl/immutable_string.hpp`: Kept GC_malloc instead of regular malloc
- `include/cpp/clojure/core_native.hpp`
- `include/cpp/clojure/string_native.hpp`
- `include/cpp/jank/analyze/expr/call.hpp`
- `include/cpp/jank/analyze/expr/var_deref.hpp`
- `include/cpp/jank/codegen/processor.hpp`
- `include/cpp/jank/error/analyze.hpp`
- `include/cpp/jank/jit/processor.hpp`
- `include/cpp/jank/runtime/context.hpp`
- `include/cpp/jank/runtime/convert/builtin.hpp`
- `include/cpp/jank/runtime/core.hpp`
- `include/cpp/jank/runtime/core/math.hpp`
- `include/cpp/jank/runtime/ns.hpp`
- `include/cpp/jank/runtime/oref.hpp`
- `include/cpp/jank/util/cli.hpp`

#### Source Files (23 files)
All source files preserved nrepl-4 version to maintain iOS/WASM/nREPL functionality:
- `src/cpp/clojure/core_native.cpp`
- `src/cpp/jank/analyze/cpp_util.cpp`
- `src/cpp/jank/analyze/processor.cpp`
- `src/cpp/jank/aot/processor.cpp`
- `src/cpp/jank/c_api.cpp`
- `src/cpp/jank/codegen/llvm_processor.cpp`
- `src/cpp/jank/codegen/processor.cpp`
- `src/cpp/jank/compiler_native.cpp`
- `src/cpp/jank/evaluate.cpp`
- `src/cpp/jank/jit/processor.cpp`
- `src/cpp/jank/read/parse.cpp`
- `src/cpp/jank/read/reparse.cpp`
- `src/cpp/jank/runtime/context.cpp`
- `src/cpp/jank/runtime/core/meta.cpp`
- `src/cpp/jank/runtime/obj/big_decimal.cpp`
- `src/cpp/jank/runtime/obj/big_integer.cpp`
- `src/cpp/jank/runtime/obj/transient_array_map.cpp`
- `src/cpp/jank/runtime/obj/transient_sorted_set.cpp`
- `src/cpp/jank/runtime/var.cpp`
- `src/cpp/jank/util/cli.cpp`
- `src/cpp/jtl/string_builder.cpp`
- `src/cpp/main.cpp`
- `test/cpp/main.cpp`

#### Clojure Source Files
- `src/jank/clojure/core.jank`: Kept nrepl-4 version
- `lein-jank/src/leiningen/jank.clj`: Kept nrepl-4 version

## New Features Integrated from origin/main

1. **GC Profiling Support**: Added `jank_profile_gc` option and valgrind tracking
2. **Phase 2 Forcing**: Added `jank_force_phase_2` option to force linking core libs in debug builds
3. **Phase 2 Output Variables**: Added proper phase 2 output path handling
4. **Build Book Workflow**: New mdBook documentation build workflow
5. **Documentation**: New book/ directory with comprehensive documentation
6. **CONTRIBUTING.md**: New contribution guidelines

## Features Preserved from nrepl-4

1. **iOS Support**: Full iOS JIT and non-JIT compilation support
2. **WASM Support**: WebAssembly target compilation support
3. **nREPL Integration**: nREPL server modules (jank.nrepl-server.core, jank.nrepl-server.server)
4. **iOS/WASM Conditional Compilation**: Proper #ifdef guards throughout codebase
5. **GC Memory Management**: Preserved GC_malloc usage for better memory management
6. **Export Module**: jank.export module for library exports

## Verification

All conflicts resolved successfully:
- Total files with conflicts: 45
- Files resolved: 45
- Conflicts remaining: 0

## Next Steps

1. Commit the merge
2. Run tests to ensure no regressions: `./bin/test`
3. Verify iOS/WASM builds still work
4. Test nREPL functionality

## Important Notes

- The nrepl-4 branch contains critical iOS/WASM functionality that wasn't in origin/main
- GC_malloc usage is intentional and important for jank's memory management
- Phase 2 build system from origin/main has been integrated with nrepl-4's module system
- All conditional compilation for iOS/WASM has been preserved
