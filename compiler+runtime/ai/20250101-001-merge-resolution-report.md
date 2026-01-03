# Merge Conflict Resolution Report - nrepl-4 <- origin/main
Date: 2025-01-01

## Summary
Resolving merge conflicts between nrepl-4 (which adds iOS/WASM/nREPL support) and origin/main (which has improvements and new features like GC profiling and phase-2 forcing).

## Resolution Strategy
**CRITICAL RULE**: Preserve ALL functionalities from BOTH sides
- nrepl-4 features: iOS/WASM conditional compilation, nREPL modules, compile server
- origin/main features: GC profiling, phase-2 forcing, JIT improvements

## Files Resolved ✅

### Build System
1. **CMakeLists.txt** - 8 conflicts
   - ✅ Kept iOS warning suppressions (`-Wno-mismatched-tags -Wno-invalid-offsetof`)
   - ✅ Added `jank_force_phase_2` option from origin/main
   - ✅ Kept Clang JIT environment setup from origin/main
   - ✅ Kept conditional nanobench (wrapped in `if(NOT jank_target_wasm AND NOT jank_target_ios)`)
   - ✅ Merged jank_lib_standalone_deps with conditional logic for iOS/WASM
   - ✅ Kept iOS/WASM conditional includes and sources
   - ✅ Kept `jank_core_library_objects` (multiple modules) vs `jank_clojure_core_output` (single)
   - ✅ Kept nrepl-4's multi-module core library compilation

2. **bin/ar-merge** - 1 conflict
   - ✅ Kept dynamic finding of core library .o files (supports multiple nREPL modules)

3. **cmake/dependency/bdwgc.cmake** - 3 conflicts
   - ✅ Added iOS/WASM -fPIC flags
   - ✅ Kept conditional thread settings for WASM (OFF) vs iOS/desktop (ON)
   - ✅ Kept TLA and parallel marking optimizations
   - ✅ Conditional gctba (OFF for WASM/iOS, ON for desktop)
   - ✅ Added `jank_profile_gc` → `enable_valgrind_tracking` from origin/main

4. **cmake/summary.cmake** - 1 conflict
   - ✅ Used origin/main's better-aligned format
   - ✅ Includes both `jank_enable_phase_2` and `jank_profile_gc`

5. **.gitignore**
   - ✅ Already resolved

### Headers
6. **include/cpp/jank/prelude.hpp** - 1 conflict
   - ✅ Kept `#include <jank/profile/time.hpp>` from nrepl-4

7. **include/cpp/jank/util/try.hpp** - 1 conflict
   - ✅ Kept full iOS/WASM conditional compilation
   - ✅ WASM/iOS use simple try/catch, desktop uses CPPTRACE

## Remaining Files (45) - Pattern Analysis

### Workflow Files (3) - CI Configuration
- .github/workflows/build-compiler+runtime.yml
- .github/workflows/build.yml
- .github/workflows/ci.yml
**Pattern**: nrepl-4 has workflows for nREPL/compile-server testing

### Headers (15) - Conditional Compilation
Most add `#ifdef JANK_TARGET_IOS` / `#ifdef JANK_TARGET_WASM` / `#ifdef JANK_NO_JIT`
- clojure/core_native.hpp
- clojure/string_native.hpp
- jank/analyze/expr/call.hpp
- jank/analyze/expr/var_deref.hpp
- jank/codegen/processor.hpp
- jank/error/analyze.hpp
- jank/jit/processor.hpp
- jank/runtime/context.hpp
- jank/runtime/convert/builtin.hpp
- jank/runtime/core.hpp
- jank/runtime/core/math.hpp
- jank/runtime/ns.hpp
- jank/runtime/oref.hpp
- jank/util/cli.hpp
- jtl/immutable_string.hpp

### Source Files (23) - Implementation
Implementations of conditional features:
- clojure/core_native.cpp
- jank/analyze/cpp_util.cpp
- jank/analyze/processor.cpp
- jank/aot/processor.cpp
- jank/c_api.cpp
- jank/codegen/llvm_processor.cpp
- jank/codegen/processor.cpp
- jank/compiler_native.cpp
- jank/evaluate.cpp
- jank/jit/processor.cpp
- jank/read/parse.cpp
- jank/read/reparse.cpp
- jank/runtime/context.cpp
- jank/runtime/core/meta.cpp
- jank/runtime/obj/big_decimal.cpp
- jank/runtime/obj/big_integer.cpp
- jank/runtime/obj/transient_array_map.cpp
- jank/runtime/obj/transient_sorted_set.cpp
- jank/runtime/var.cpp
- jank/util/cli.cpp
- jtl/string_builder.cpp
- main.cpp
- test/cpp/main.cpp

### Other (4)
- src/jank/clojure/core.jank
- test/bash/clojure-test-suite/clojure-test-suite
- lein-jank/src/leiningen/jank.clj

## Resolution Strategy for Remaining Files
For each file:
1. Identify conflict type (conditional compilation, feature addition, refactoring)
2. Preserve iOS/WASM/nREPL conditionals from nrepl-4
3. Add any new functionality from origin/main
4. Ensure both code paths are preserved

## Key Principles Applied
- iOS/WASM builds use stub implementations (no JIT, no cpptrace)
- Desktop builds get full features
- nREPL modules system preserved
- All origin/main improvements added where compatible
