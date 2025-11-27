# Plan: Make All clojure.* Libraries Available to WASM jank

## Overview
Currently, WASM jank has support for `clojure.core` and `clojure.set`. We need to make all libraries in `compiler+runtime/src/jank/clojure/` available to WASM builds.

## Implementation Status (Updated)

### ✅ Fully Supported
1. **clojure.core** (`core.jank`)
   - Pre-compiled to C++ via `jank run --codegen wasm-aot`
   - Loaded via `jank_load_core()` in `emscripten-bundle`
   - Native setup via `jank_setup_clojure_core_for_wasm()`

2. **clojure.set** (`set.jank`)
   - Pre-compiled to C++ via `jank run --codegen wasm-aot`
   - Loaded via `jank_load_set()` in `emscripten-bundle`
   - Regenerated when source changes
   - ✅ WORKING

3. **clojure.walk** (`walk.jank`)
   - Pure jank implementation
   - No native dependencies
   - Infrastructure added to emscripten-bundle
   - ✅ READY (pending compilation test)

4. **clojure.template** (`template.jank`)
   - Pure jank implementation
   - Depends on clojure.walk
   - Infrastructure added to emscripten-bundle
   - ✅ READY (pending compilation test)

5. **clojure.string** (`string.jank`)
   - Uses native C++ functions via `cpp/clojure.string_native.*`
   - Uses reader conditionals (`#?(:wasm ...)`) for WASM-specific code paths
   - WASM version uses string delimiters instead of regex patterns
   - ✅ WORKING
   - Functions available:
     - `upper-case`, `lower-case`, `capitalize`, `reverse`
     - `trim`, `triml`, `trimr`, `trim-newline`
     - `blank?`, `join`, `escape`
     - `split` (with string delimiter), `split-lines`
     - `replace` (string replacement only)
     - `replace-first` (string replacement only)
     - `starts-with?`, `ends-with?`, `includes?`
     - `index-of`, `last-index-of`
   - Note: Regex-based functions are not available in WASM (use string delimiters instead)

6. **clojure.test** (`test.jank`)
   - Pure jank implementation (depends on clojure.template)
   - Provides full testing framework in WASM
   - ✅ WORKING
   - Features available:
     - `deftest`, `testing`, `is`, `are`
     - `run-tests`, `run-test`, `run-all-tests`
     - `thrown?` assertions
     - Fixtures with `use-fixtures`
     - Test reporting with `:pass`, `:fail`, `:error`
   - Note: No regex-based `thrown-with-msg?` in WASM (regex not supported)

## Implementation Plan

### Phase 1: clojure.string (Highest Priority)

#### Step 1.1: Verify string_native C++ implementation exists
- Location: Should be in `compiler+runtime/src/cpp/clojure/string_native.cpp`
- Check if it's already part of WASM build (in libjank.a)

#### Step 1.2: Add clojure.string to emscripten-bundle
Add after clojure.set loading (around line 238):

```bash
# Regenerate clojure.string if needed
clojure_string_cpp="${output_dir}/clojure_string_generated.cpp"
clojure_string_jank="${repo_root}/src/jank/clojure/string.jank"
if [[ -f "${clojure_string_jank}" ]]; then
  regenerate_core_lib "clojure.string" "${clojure_string_cpp}" "${clojure_string_jank}"
fi

if [[ -f "${clojure_string_cpp}" ]]; then
  echo "[emscripten-bundle] Found pre-compiled clojure.string: ${clojure_string_cpp}"
  compiled_objects+=("${clojure_string_cpp}")
fi
```

#### Step 1.3: Add loader call in entrypoint template
Add after clojure.set loading (around line 440):

```cpp
// Load clojure.string if available
printf("[jank-wasm] Loading clojure.string...\\n");
jank_load_string();
__rt_ctx->module_loader.set_is_loaded("clojure.string");
printf("[jank-wasm] clojure.string loaded!\\n");
```

#### Step 1.4: Add extern declaration
Add near line 386:

```cpp
void* jank_load_string();
```

### Phase 2: clojure.walk

#### Step 2.1: Add clojure.walk to emscripten-bundle
Similar to clojure.string:

```bash
# Regenerate clojure.walk if needed
clojure_walk_cpp="${output_dir}/clojure_walk_generated.cpp"
clojure_walk_jank="${repo_root}/src/jank/clojure/walk.jank"
if [[ -f "${clojure_walk_jank}" ]]; then
  regenerate_core_lib "clojure.walk" "${clojure_walk_cpp}" "${clojure_walk_jank}"
fi

if [[ -f "${clojure_walk_cpp}" ]]; then
  echo "[emscripten-bundle] Found pre-compiled clojure.walk: ${clojure_walk_cpp}"
  compiled_objects+=("${clojure_walk_cpp}")
fi
```

#### Step 2.2: Add loader call and extern
```cpp
extern "C" {
  void* jank_load_walk();
}

// In jank_main:
printf("[jank-wasm] Loading clojure.walk...\\n");
jank_load_walk();
__rt_ctx->module_loader.set_is_loaded("clojure.walk");
printf("[jank-wasm] clojure.walk loaded!\\n");
```

### Phase 3: clojure.template

#### Step 3.1: Add clojure.template to emscripten-bundle
Similar pattern:

```bash
# Regenerate clojure.template if needed
clojure_template_cpp="${output_dir}/clojure_template_generated.cpp"
clojure_template_jank="${repo_root}/src/jank/clojure/template.jank"
if [[ -f "${clojure_template_jank}" ]]; then
  regenerate_core_lib "clojure.template" "${clojure_template_cpp}" "${clojure_template_jank}"
fi

if [[ -f "${clojure_template_cpp}" ]]; then
  echo "[emscripten-bundle] Found pre-compiled clojure.template: ${clojure_template_cpp}"
  compiled_objects+=("${clojure_template_cpp}")
fi
```

#### Step 3.2: Add loader call (must be after walk)
```cpp
extern "C" {
  void* jank_load_template();
}

// In jank_main (AFTER walk):
printf("[jank-wasm] Loading clojure.template...\\n");
jank_load_template();
__rt_ctx->module_loader.set_is_loaded("clojure.template");
printf("[jank-wasm] clojure.template loaded!\\n");
```

### Phase 4: clojure.test (Future/Optional)

#### Step 4.1: Consider if test is needed in WASM
- Useful for running jank tests in browser/Node.js
- Large library - may increase WASM bundle size significantly
- Decision: Implement last, make optional via flag

#### Step 4.2: If implementing, follow same pattern as above

## Implementation Strategy

### Refactoring Opportunity
Instead of manually adding each library, we can:

1. **Create a library list** in `emscripten-bundle`:
```bash
# List of core libraries to pre-compile
CORE_LIBS=(
  "core:clojure/core.jank"
  "set:clojure/set.jank"
  "string:clojure/string.jank"
  "walk:clojure/walk.jank"
  "template:clojure/template.jank"
)
```

2. **Loop through libraries**:
```bash
for lib_spec in "${CORE_LIBS[@]}"; do
  lib_name="${lib_spec%%:*}"
  lib_path="${lib_spec#*:}"
  lib_munged="${lib_name//./_}"

  lib_cpp="${output_dir}/clojure_${lib_munged}_generated.cpp"
  lib_jank="${repo_root}/src/jank/${lib_path}"

  if [[ -f "${lib_jank}" ]]; then
    regenerate_core_lib "clojure.${lib_name}" "${lib_cpp}" "${lib_jank}"
  fi

  if [[ -f "${lib_cpp}" ]]; then
    echo "[emscripten-bundle] Found pre-compiled clojure.${lib_name}: ${lib_cpp}"
    compiled_objects+=("${lib_cpp}")
  fi
done
```

3. **Generate entrypoint dynamically**:
```cpp
// Auto-generated library loaders
$(for lib_spec in "${CORE_LIBS[@]}"; do
  lib_name="${lib_spec%%:*}"
  lib_munged="${lib_name//./_}"
  echo "extern void* jank_load_${lib_munged}();"
done)

// In jank_main:
$(for lib_spec in "${CORE_LIBS[@]}"; do
  lib_name="${lib_spec%%:*}"
  lib_munged="${lib_name//./_}"
  cat <<LOAD_LIB
    printf("[jank-wasm] Loading clojure.${lib_name}...\\\\n");
    jank_load_${lib_munged}();
    __rt_ctx->module_loader.set_is_loaded("clojure.${lib_name}");
    printf("[jank-wasm] clojure.${lib_name} loaded!\\\\n");
LOAD_LIB
done)
```

## Testing Plan

### Test 1: Basic Import Test
Create `test_libs.jank`:
```clojure
(ns test-libs
  (:require
   [clojure.set :as set]
   [clojure.string :as str]
   [clojure.walk :as walk]
   [clojure.template :as template]))

(println "Testing clojure.set:")
(println (set/union #{1 2} #{2 3}))

(println "\nTesting clojure.string:")
(println (str/upper-case "hello"))
(println (str/join ", " ["a" "b" "c"]))

(println "\nTesting clojure.walk:")
(println (walk/postwalk-replace {:a :A} {:a 1 :b {:a 2}}))

(println "\nAll libraries loaded successfully!")
```

### Test 2: Function Tests
```clojure
(defn test-string-ops []
  (assert (= "HELLO" (str/upper-case "hello")))
  (assert (= "hello" (str/lower-case "HELLO")))
  (assert (= ["a" "b" "c"] (str/split "a,b,c" #",")))
  (println "String tests passed!"))

(defn test-walk-ops []
  (assert (= {:A 1 :b {:A 2}}
             (walk/postwalk-replace {:a :A} {:a 1 :b {:a 2}})))
  (println "Walk tests passed!"))
```

### Test 3: Integration Test
Run existing `eita.jank` which already uses clojure.set and clojure.string

## File Changes Summary

### Files to Modify
1. **`compiler+runtime/bin/emscripten-bundle`**
   - Add library regeneration for string, walk, template
   - Add loader extern declarations
   - Add loader calls in entrypoint
   - Recommended: Refactor to use loop-based approach

### Files to Verify
1. **`compiler+runtime/src/cpp/clojure/string_native.cpp`**
   - Ensure exists and compiles for WASM target
   - Check for any platform-specific code

2. **`compiler+runtime/src/jank/clojure/*.jank`**
   - Verify all compile with `--codegen wasm-aot`
   - Check for any JVM-specific features

## Success Criteria

- [ ] `clojure.string` functions work in WASM
- [ ] `clojure.walk` functions work in WASM
- [ ] `clojure.template` macros work in WASM
- [ ] `eita.jank` still works with new changes
- [ ] New test file using all libraries works
- [ ] No increase in link time for user code (prelinked runtime handles libs)
- [ ] Documentation updated

## Potential Issues

### Issue 1: clojure.string native dependencies
- **Problem**: May depend on regex or other JVM features
- **Solution**: Check string_native.cpp for WASM compatibility
- **Mitigation**: Add `#ifndef JANK_TARGET_WASM` guards if needed

### Issue 2: Module loading order
- **Problem**: clojure.template depends on clojure.walk
- **Solution**: Ensure walk is loaded before template
- **Verification**: Test dependency chain

### Issue 3: Bundle size increase
- **Problem**: Adding more libraries increases WASM size
- **Solution**: Use prelinked runtime to cache core libs
- **Monitoring**: Track WASM bundle size before/after

## Performance Considerations

1. **Incremental compilation**: Libraries are cached as .o files
2. **Prelinked runtime**: Core libs in prelinked runtime, only user code relinks
3. **Fast iteration**: With caching, only changed libs regenerate

## Future Enhancements

1. **Auto-discovery**: Scan `src/jank/clojure/` for all .jank files
2. **Lazy loading**: Load libraries on-demand in browser
3. **Tree shaking**: Only include used libraries
4. **Separate bundles**: Core bundle + user bundle for better caching

## Rollout Plan

1. **Phase 1**: Implement manual approach for string, walk, template
2. **Phase 2**: Test thoroughly with existing code
3. **Phase 3**: Refactor to loop-based approach
4. **Phase 4**: Add auto-discovery
5. **Phase 5**: Optimize bundle size and loading

## Dependencies Graph

```
clojure.core (no deps) ✓
  ↓
clojure.set (depends on core) ✓
clojure.string (depends on core, uses native)
clojure.walk (depends on core)
  ↓
clojure.template (depends on walk)
clojure.test (depends on core, set, string, walk, template)
```

**Load order must be**: core → set → string → walk → template → test
