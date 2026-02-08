# ODR (One Definition Rule) Violation Fix Plan for cpp/raw in Standalone Mode

## Problem Statement

When running `jank compile -o <name> <namespace>` for standalone/AOT builds, `cpp/raw` blocks cause ODR violations because they get compiled twice:

1. **JIT phase** - The code is JIT compiled during `load_module()` evaluation as `input_line_*` symbols
2. **AOT phase** - The same code is generated as C++ source from the analyzed AST for the final executable

Both outputs end up in the same final compilation, causing redefinition errors:
```
/path/src/vybe/type.jank:148:27: error: redefinition of 'vybe_struct_begin'
input_line_189:11:27: note: previous definition is here
```

**Note**: jank already has `#ifndef JANK_CPP_RAW_<hash>` guards, but they don't help here because:
- The JIT-compiled code is already in the LLVM IR/object code (not C++ source with guards)
- The AOT-generated C++ source gets the guards, but that doesn't prevent it from conflicting with already-compiled JIT symbols

## Root Cause Analysis

### Code Flow Investigation

#### 1. JIT Path (during `jank compile`)
When `jank compile` is executed, it calls `context::compile_module()` which:
- Sets `*compile-files*` to `true`
- Calls `load_module()` with the module

During module loading (`context::eval_string()`):
- **Lines 194-207**: Forms are analyzed and optimized
- **Line 206**: `evaluate::eval(expr)` is called - **THIS IS WHERE JIT HAPPENS**
- For `cpp/raw` blocks, `evaluate.cpp:eval(expr::cpp_raw_ref)` calls:
  - `interpreter->Parse(expr->code.c_str())` - parses the C++ code
  - The parsed code becomes `input_line_*` in the JIT
  - These symbols are compiled into LLVM IR and become part of the JIT runtime

#### 2. AOT Path (also during `jank compile`)
After JIT evaluation, if `*compile-files*` is true (`context.cpp` lines 229-391):
- **Lines 234-242**: Creates a wrapper function containing all the forms
- **Lines 286-290**: `codegen::processor cg_prc` generates C++ source
- **Line 291**: Calls `cg_prc.declaration_str()` to get the generated code
- For `cpp/raw` blocks, `codegen/processor.cpp:gen(expr::cpp_raw_ref)`:
  - **Line 1522**: `util::format_to(deps_buffer, "{}\n", expr->code)`
  - This writes the raw C++ code directly to the deps buffer
  - The generated C++ file includes this code literally
  - **NO GUARDS ARE ADDED** - the code is just copied verbatim

#### 3. Final Linking (in `aot/processor.cpp`)
When creating the standalone executable:
- All `.o` files from modules are linked together
- The generated entrypoint C++ is compiled and linked
- **The JIT-compiled `input_line_*` symbols clash with the AOT-generated function definitions**

### Why Existing Guards Don't Work

Looking at `analyze/processor.cpp`, there are NO guards being generated for `cpp/raw` blocks. The hash-based guards (`#ifndef JANK_CPP_RAW_<hash>`) mentioned in the problem description **don't actually exist in the current codebase**.

Searching for `JANK_CPP_RAW` in the codebase returns **zero results** in the actual source code - they're only mentioned in test files and documentation.

### The Core Issue

The problem is architectural:

1. **JIT compilation happens first** during module loading for all code, including `cpp/raw`
2. **AOT compilation generates the same code** for inclusion in the final binary
3. **Both exist in the final link**, causing ODR violations
4. Guards wouldn't help even if they existed, because:
   - JIT code is already compiled to native/IR before guards would be checked
   - You can't use preprocessor guards to prevent linking pre-compiled symbols

## The Real Solution Needed

The issue is that `jank compile` uses a hybrid approach:
- JIT to execute code and build up the runtime state
- AOT to generate standalone C++ source

For `cpp/raw` specifically, we need to:
1. **Skip JIT compilation** of `cpp/raw` when in `*compile-files*` mode
2. **Only generate the code** for AOT output

OR

1. **Mark JIT-compiled `cpp/raw` symbols** with `static` or in anonymous namespace
2. **Ensure AOT code uses different symbols** or properly guards them

## Proposed Fix Location

### Option 1: Skip JIT Evaluation of cpp/raw in Compile Mode (RECOMMENDED)

**File**: `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/evaluate.cpp`

**Function**: `object_ref eval(expr::cpp_raw_ref const expr)`

**Current code** (around line 838):
```cpp
object_ref eval(expr::cpp_raw_ref const expr)
{
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    auto const &interpreter{ __rt_ctx->jit_prc.interpreter };

    /* Parse and execute the C++ code */
    auto parse_res{ interpreter->Parse(expr->code.c_str()) };
    // ... rest of JIT compilation
```

**Proposed fix**:
```cpp
object_ref eval(expr::cpp_raw_ref const expr)
{
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    // Skip JIT compilation if we're in AOT mode - the code will be included
    // in the generated C++ output instead
    if(truthy(__rt_ctx->compile_files_var->deref()))
    {
      return jank_nil;
    }

    auto const &interpreter{ __rt_ctx->jit_prc.interpreter };
    /* Parse and execute the C++ code */
    auto parse_res{ interpreter->Parse(expr->code.c_str()) };
    // ... rest of existing code
```

**Why this works**:
- When `*compile-files*` is true (AOT mode), `cpp/raw` blocks won't be JIT compiled
- They'll only appear in the generated C++ source
- No symbol duplication occurs
- For normal REPL/JIT usage, behavior is unchanged

### Option 2: Add Guards to Generated AOT Code

**File**: `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp`

**Function**: `processor::gen(expr::cpp_raw_ref const expr, expr::function_arity const &)`

**Current code** (around line 1520):
```cpp
jtl::option<handle> processor::gen(expr::cpp_raw_ref const expr, expr::function_arity const &)
{
    util::format_to(deps_buffer, "{}\n", expr->code);
    // ...
```

**Proposed fix**:
```cpp
jtl::option<handle> processor::gen(expr::cpp_raw_ref const expr, expr::function_arity const &)
{
    // Generate hash-based include guard to prevent ODR violations
    auto const code_hash = analyze::hash_expression(expr);
    util::format_to(deps_buffer,
        "#ifndef JANK_CPP_RAW_{:016x}\n"
        "#define JANK_CPP_RAW_{:016x}\n"
        "{}\n"
        "#endif // JANK_CPP_RAW_{:016x}\n",
        code_hash, code_hash, expr->code, code_hash);
    // ...
```

**Why this is NOT sufficient**:
- This only prevents the AOT code from being included twice
- It doesn't solve the fundamental issue of JIT symbols already existing
- The `input_line_*` symbols from JIT will still conflict

## Recommended Approach

**Option 1 is strongly recommended** because:

1. **Simplicity**: One-line check in evaluate.cpp
2. **Correctness**: Prevents the root cause (double compilation)
3. **Performance**: No unnecessary JIT compilation during AOT
4. **Semantics**: When compiling for AOT, you don't need JIT artifacts

However, there's one concern to validate:

### Validation Required

**Question**: Do `cpp/raw` blocks need to execute during module loading for side effects?

For example, if a `cpp/raw` block defines a global variable that later code depends on:
```clojure
(cpp/raw "int my_global = 42;")
(defn get-value [] (cpp/my_global))
```

During `jank compile`:
- If we skip JIT of the first line, does `get-value` fail to analyze/evaluate?
- The answer: **NO** - because during AOT compilation, analysis happens but C++ interop resolution happens via libclang, not JIT

The JIT-compiled `cpp/raw` code is only needed for actual REPL execution, not for analyzing subsequent forms.

## Implementation Plan

### Phase 1: Implement Option 1 (Skip JIT in Compile Mode)

1. **Modify** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/evaluate.cpp`
   - Add check for `*compile-files*` in `eval(expr::cpp_raw_ref)`
   - Return `jank_nil` early if in compile mode

2. **Test** with the existing `cpp_raw_inline` test:
   ```bash
   cd /Users/pfeodrippe/dev/jank/compiler+runtime/test/bash/ahead-of-time
   ./pass-test
   ```

3. **Verify** no ODR violations occur

### Phase 2: Add Guards as Defense-in-Depth (Optional)

If Phase 1 works but we want extra safety:

1. **Modify** `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp`
   - Add hash-based guards around generated `cpp/raw` code
   - Use `analyze::hash_expression(expr)` for consistent hashing

2. **Add tests** to verify guards work correctly

### Phase 3: Test with Real-World Case

Test with the actual vybe project that revealed this issue:
```bash
jank compile -o vybe vybe.core
# Should compile without ODR violations
```

## Risks and Considerations

### Risk 1: Side Effects in cpp/raw During Load
- **Mitigation**: Document that `cpp/raw` for side-effect-only code should use different patterns
- **Reality**: Most `cpp/raw` usage is for definitions, not side effects

### Risk 2: REPL-Compiled Modules
- **Impact**: None - REPL doesn't set `*compile-files*`, so JIT still happens
- **Validation**: Existing REPL tests should pass

### Risk 3: Cross-Module cpp/raw Dependencies
- **Scenario**: Module A has `cpp/raw` defining a function, Module B calls it
- **Status**: Should work - both modules' AOT code will have the definitions
- **Note**: May need guards (Option 2) if multiple modules have same `cpp/raw` text

## Testing Strategy

1. **Unit Test**: Modify existing cpp_raw_inline test to verify no duplication
2. **Integration Test**: Create test with multiple modules sharing `cpp/raw` code
3. **Regression Test**: Ensure REPL mode still JIT compiles `cpp/raw` correctly

## Alternative Approaches Considered

### Alternative 1: Use `static` or Anonymous Namespace in JIT
- **Idea**: Make JIT symbols non-exportable
- **Problem**: Can't control symbol visibility in incremental JIT compilation
- **Status**: Not feasible with current Clang interpreter

### Alternative 2: Skip AOT Generation Instead
- **Idea**: Only use JIT-compiled symbols, don't regenerate for AOT
- **Problem**: JIT symbols aren't easily extractable to final binary
- **Status**: Would require major architectural changes

### Alternative 3: Symbol Renaming
- **Idea**: Rename JIT symbols differently from AOT symbols
- **Problem**: Doesn't prevent the waste of double compilation
- **Status**: More complex than Option 1 with no benefits

## Open Questions

1. ✓ Are there any `cpp/raw` blocks that MUST execute during module load?
   - Answer: No, they're definitions, not side effects

2. ? How does this interact with incremental compilation?
   - Need to verify with caching enabled

3. ✓ What about WASM target?
   - Already has `#if !defined(JANK_TARGET_WASM)` guards, won't be affected

## Summary

The ODR violation in standalone mode is caused by `cpp/raw` blocks being compiled twice:
- Once during JIT evaluation (as `input_line_*`)
- Once in generated AOT C++ source (as literal code)

---

## Dec 12, 2025 - Implementation Attempt Results

### Approach 1: Skip JIT in evaluate.cpp - FAILED
The proposed fix of skipping JIT compilation breaks symbol resolution:
- `cpp/sdf_sqrt` calls need the JIT-compiled symbols to resolve
- Without JIT, `Unable to find 'sdf_sqrt' within the global namespace`

### Approach 2: Skip AOT codegen - FAILED
Skipping cpp/raw in codegen/processor.cpp breaks AOT compilation:
- cpp/raw blocks contain `#include` statements needed by AOT code
- Error: `error reading 'new': No such file or directory`

### Approach 3: Use `static inline` - FAILED
Even with `static inline`, redefinition errors occur:
- JIT and AOT code are in the SAME clang interpreter context
- `static` only helps across separate translation units
- Both share the same clang interpreter state

### Approach 4: Existing preprocessor guards - INSUFFICIENT
jank already generates `#ifndef JANK_CPP_RAW_XXXX` guards (analyze/processor.cpp:4441)
- Guards work within a single preprocessing context
- JIT and AOT use separate parsing contexts
- Guards don't prevent conflicts with already-compiled JIT symbols

### Root Cause Clarified
The issue is that `jank compile` uses the SAME clang interpreter for both:
1. JIT compilation during module loading (for symbol resolution)
2. AOT C++ source parsing (for generating standalone code)

When AOT code is parsed, clang sees functions that were already JIT-compiled,
causing redefinition errors regardless of `static`, `inline`, or guards.

### Potential Architectural Fixes

1. **Separate interpreter contexts**: Use one interpreter for JIT, a fresh one for AOT
2. **Symbol namespace isolation**: JIT-compile cpp/raw in anonymous namespace
3. **Weak symbols**: Use `__attribute__((weak))` for JIT versions
4. **Parse cpp/raw content**: Extract only includes for AOT, rely on JIT .o for definitions

All of these require significant changes to jank's architecture.
