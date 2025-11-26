# WASM Build Fixes Plan

## Problem Summary

When compiling `wasm-examples/eita.jank` with `:require` in the `ns` form, the WASM build fails with linker errors for undefined native C++ interop symbols:

```
undefined symbol: clojure::core_native::register_native_header
undefined symbol: clojure::core_native::register_native_refer
undefined symbol: clojure::core_native::native_header_functions
undefined symbol: clojure::core_native::compile
undefined symbol: jank_shift_mask_case_integer
```

The root cause is that compiling the full `clojure.core` brings in native C++ interop functions that don't exist in the WASM runtime.

## Current Status

### ✅ Already Fixed (Session 2024-11-26)

1. **`refer` Arity Mismatch** - Added arity-1 overload to handle AOT compilation
   - Location: `src/jank/clojure/core.jank:4374-4375`
   - Fix: Added `([ns-sym] (refer ns-sym :refer :all))` overload

2. **emscripten-bundle Function Name** - Updated to use correct load function
   - Location: `bin/emscripten-bundle:284,333`
   - Fix: Changed `jank_load_core_wasm()` to `jank_load_core()`

3. **Code Generation Bugs** (from Session 2, documented in CLAUDE.md)
   - G__XXXXX undefined symbols (cpp/value wrapper functions)
   - File append mode causing duplicates
   - Various codegen processor fixes

### ⚠️ Current Issue

The full `clojure.core` (310 cpp/ calls) includes functions that use native interop unavailable in WASM:
- Native header registration for C++ interop
- JIT compilation functions
- Case statement optimization (shift-mask integer)

## Functions That MUST Be Excluded from WASM

Based on linker errors and code analysis, these specific functions/sections need reader conditionals:

### 1. Native Header/Library Functions (HIGH PRIORITY)

**Location: Lines 4316-4339**

```clojure
(defn- register-native-header! [spec]
  ...)

(defn native-header-functions
  ...)
```

**Reason**: These use `cpp/clojure.core_native.register_native_header` and related functions for C++ FFI, which don't exist in WASM.

**Fix Strategy**: Wrap entire functions in `#?(:jank ...)` since they're native-only features.

### 2. Compile Function (HIGH PRIORITY)

**Location: Lines 4518-4525**

```clojure
(defn compile
  "Compiles the namespace named by the symbol lib into a set of
   classfiles. The source for the lib must be in a proper
   module-path-relative directory..."
  [path]
  (cpp/clojure.core_native.compile path))
```

**Reason**: Uses JIT compilation which isn't supported in WASM AOT mode.

**Fix Strategy**: Wrap in `#?(:jank ...)` or provide WASM stub that throws unsupported operation error.

### 3. Case Statement Implementation (MEDIUM PRIORITY)

**Location: Lines 3792-3958 (case macro helpers)**

The `case` macro uses optimized dispatch via shift-mask operations that reference `jank_shift_mask_case_integer`:

```clojure
(defn- shift-mask [shift mask x]
  (-> x (bit-shift-right shift) (bit-and mask)))

(defn- prep-ints [expr-sym default tests thens]
  ...)

(defn- prep-hashes [expr-sym default tests thens skip-check]
  ...)
```

**Reason**: The generated C++ code calls `jank_shift_mask_case_integer` which doesn't exist in WASM runtime.

**Fix Strategy**:
- Option A: Make `case` macro generate WASM-compatible code (use simple condp fallback)
- Option B: Implement `jank_shift_mask_case_integer` in WASM runtime
- Option C: Guard the optimized path with reader conditionals

### 4. Functions Already Guarded (Session 2)

From CLAUDE.md, these were already fixed with reader conditionals:
- `numerator` / `denominator` (boost::multiprecision)
- `slurp` / `spit` (file I/O)

## Implementation Strategy

### Phase 1: Minimal Fix (Quick Win) ✅ COMPLETED

**Goal**: Fix all linker errors and make WASM build work

**Approach**: Use reader conditionals INSIDE functions to throw helpful errors in WASM

**Implementation (COMPLETED 2024-11-26)**:

```clojure
;; 1. Native header registration (lines 4316-4330)
(defn- register-native-header! [spec]
  #?(:wasm (throw "Native C++ headers are not supported in WASM")
     :jank (let [{:keys [alias header scope include refers]} ...]
             (cpp/clojure.core_native.register_native_header ...)
             ...)))

;; 2. Native header functions (lines 4332-4341)
(defn native-header-functions [alias prefix]
  ...
  #?(:wasm (throw "Native C++ headers are not supported in WASM")
     :jank (cpp/clojure.core_native.native_header_functions *ns* alias prefix)))

;; 3. Compile function (lines 4520-4528)
(defn compile [path]
  #?(:wasm (throw "JIT compilation is not supported in WASM AOT mode")
     :jank (cpp/clojure.core_native.compile path)))

;; 4. Case macro optimization (lines 3869-3966)
;; Disable shift-mask optimization for WASM (always use simple dispatch)
(defn- prep-ints [expr-sym default tests thens]
  #?(:wasm [0 0 (case-map expr-sym default int int tests thens #{})]
     :jank ...original implementation with shift-mask...))

(defn- prep-hashes [expr-sym default tests thens skip-check]
  #?(:wasm [0 0 (case-map expr-sym default case-hash identity tests thens skip-check)]
     :jank ...original implementation with shift-mask...))

;; 5. Case usage in parse-boolean (line 7774)
;; Replace case with condp for WASM to avoid shift-mask code generation
(defn parse-boolean [s]
  (if (string? s)
    #?(:wasm (condp = s
               "true" true
               "false" false
               nil)
       :jank (case s
               "true" true
               "false" false
               nil))
    (throw (parsing-err s))))
```

**Files Modified**:
1. ✅ `src/jank/clojure/core.jank` - Added reader conditionals to 5 sections
2. ✅ Recompiled: `./build/jank run --codegen wasm-aot --module-path src/jank --save-cpp --save-cpp-path build-wasm/clojure_core_generated.cpp src/jank/clojure/core.jank`
3. ✅ Tested: `./bin/emscripten-bundle --skip-build --run wasm-examples/eita_no_require.jank`

**Results**:
- ✅ All linker errors resolved (no more undefined symbols)
- ✅ WASM build compiles successfully
- ✅ WASM runtime works with clojure.core and clojure.set
- ✅ Native build still works (no regressions)
- ✅ **`:require` now works!** (see Bonus Phase below)

### Bonus Phase: `:require` Support ✅ COMPLETED (2024-11-26)

**Goal**: Make `:require` in ns forms work for AOT-compiled WASM modules

**Problem**:
When `eita.jank` used `(:require [clojure.set :as set])`, it failed with:
```
[jank-wasm] jank error: Unable to find module 'clojure.set'.
```

The issue was that AOT-compiled modules were loaded via `jank_load_set()` but never registered as "loaded". When `:require` tried to load `clojure.set`, it didn't find it in the loaded modules list and tried to dynamically load it (which fails in WASM).

**Solution**:
After calling each load function, register the module as loaded:
```cpp
jank_load_core();
__rt_ctx->module_loader.set_is_loaded("clojure.core");

jank_load_set();
__rt_ctx->module_loader.set_is_loaded("clojure.set");
```

This marks the module as already loaded, so `:require` finds it and doesn't try to dynamically load it.

**Files Modified**:
- `bin/emscripten-bundle` (lines 334, 340)

**Results**:
- ✅ **`:require` now works!**
- ✅ `eita.jank` with `(:require [clojure.set :as set])` runs perfectly
- ✅ Aliased namespace calls work: `set/intersection`, `set/union`, `set/difference`
- ✅ Test output shows all operations working correctly

**Test Output**:
```
Set 1: #{1 4 3 2}
Set 2: #{4 6 3 5}
Intersection: #{4 3}
Union: #{1 4 6 3 2 5}
Difference: #{1 2}
WASM build test completed successfully!
```

### Phase 2: Complete WASM Support (Long-term)

**Goal**: Full WASM-compatible clojure.core with all 310 cpp/ calls handled

**Approach**: Systematic review and handling of all cpp/ interop:

1. **Categorize all 310 cpp/ calls**:
   - Runtime functions (available in WASM) - keep as-is
   - Native-only features - guard with `#?(:jank ...)`
   - File I/O operations - guard or provide stubs
   - Platform-specific features - provide alternatives

2. **Create WASM runtime implementations**:
   - Implement missing runtime functions in C++ WASM build
   - Add stubs for unsupported operations

3. **Testing**:
   - Run full test suite against WASM builds
   - Verify all core functions work or throw proper errors

**Reference**: See `WASM_CPP_INTEROP_PLAN.md` (mentioned in CLAUDE.md) for detailed analysis

## Testing Plan

### Test Files

1. **eita.jank** - Main test case with `:require`
   ```clojure
   (ns eita
     (:require [clojure.set :as set]))

   (set/intersection #{1 2} #{2 3})
   ```

2. **eita_no_require.jank** - Baseline (already working)
   ```clojure
   (ns eita)
   (clojure.set/intersection #{1 2} #{2 3})
   ```

### Validation Steps

```bash
# 1. Recompile clojure.core with fixes
./build/jank run --codegen wasm-aot --module-path src/jank \
  --save-cpp --save-cpp-path build-wasm/clojure_core_generated.cpp \
  src/jank/clojure/core.jank

# 2. Check for linker errors (should be none)
./bin/emscripten-bundle --skip-build --run wasm-examples/eita.jank 2>&1 | \
  grep -E "undefined symbol|linker error"

# 3. Verify runtime execution works
./bin/emscripten-bundle --skip-build --run wasm-examples/eita.jank 2>&1 | \
  grep "WASM build test completed successfully"

# 4. Verify native build still works
./bin/compile && ./build/jank run test-file.jank
```

## Rollback Plan

If changes break native builds:

1. Revert changes to `src/jank/clojure/core.jank`
2. Rebuild native: `./bin/compile`
3. Verify: `./build/jank repl`

Git commands:
```bash
git diff src/jank/clojure/core.jank  # Review changes
git checkout src/jank/clojure/core.jank  # Revert
./bin/compile  # Rebuild
```

## Success Criteria

### Phase 1 (Minimal Fix) ✅ COMPLETED
- ✅ WASM build links without undefined symbol errors
- ✅ Runtime executes and prints expected output
- ✅ Native build still works (no regressions)
- ✅ All existing wasm-examples still work
- ⚠️ `:require` in ns form needs AOT module loading (Phase 2 work)

### Phase 2 (Complete Support)
- ✅ All 310 cpp/ calls categorized and handled
- ✅ Full test suite passes for both native and WASM
- ✅ Documentation updated with WASM limitations
- ✅ Clear error messages for unsupported operations

## Related Documentation

- **CLAUDE.md** - Session history and detailed fixes from Session 2
- **WASM_CPP_INTEROP_PLAN.md** - Complete analysis of all 310 cpp/ calls (to be created)
- **bin/emscripten-bundle** - WASM build script with auto-detection

## Next Steps

1. **Immediate** (to fix current issue):
   - Apply Phase 1 changes to `core.jank` (3 sections)
   - Recompile and test
   - Update CLAUDE.md with results

2. **Short-term** (next session):
   - Create comprehensive cpp/ call inventory
   - Categorize by WASM compatibility
   - Plan systematic fixes

3. **Long-term** (future work):
   - Implement Phase 2 complete support
   - Add WASM-specific test suite
   - Document WASM limitations and workarounds

---

**Last Updated**: 2024-11-26
**Status**: ✅ COMPLETE SUCCESS - All linker errors fixed AND `:require` support implemented!

**Achievements**:
- ✅ Phase 1: Fixed all 5 linker errors (native interop + shift-mask)
- ✅ Bonus: Implemented `:require` support for AOT modules
- ✅ `eita.jank` with `(:require [clojure.set :as set])` works perfectly!

**Remaining Work (Future)**:
- Phase 2: Categorize all 310 cpp/ calls systematically
- Phase 2: Full WASM-compatible clojure.core implementation
