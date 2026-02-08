# ODR Violation Investigation for cpp/raw in Standalone Mode

**Date**: 2025-12-12
**Issue**: cpp/raw blocks cause ODR (One Definition Rule) violations in standalone/AOT compilation

## What I Learned

### The Problem
When running `jank compile -o <name> <namespace>`, cpp/raw blocks get compiled **twice**:
1. JIT compiled during module loading (creates `input_line_*` symbols)
2. Generated as C++ source in AOT output (creates function definitions)

Both end up in the final binary, causing redefinition errors.

### Root Cause Discovery

#### Key Files Investigated
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/runtime/context.cpp` - Module compilation orchestration
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/evaluate.cpp` - JIT evaluation of expressions
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp` - AOT code generation
- `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/aot/processor.cpp` - Standalone executable creation

#### The Flow

**JIT Phase** (context.cpp lines 194-207):
```cpp
// During eval_string() when loading a module
auto expr = an_prc.analyze(parsed_form, analyze::expression_position::statement).expect_ok();
expr = analyze::pass::optimize(expr);
ret = evaluate::eval(expr);  // <-- This JIT compiles cpp/raw
```

**AOT Phase** (context.cpp lines 229-391):
```cpp
// If *compile-files* is true (set by compile_module)
if(truthy(compile_files_var->deref()))
{
    auto const expr = analyze::pass::optimize(an_prc.analyze(form, ...));
    codegen::processor cg_prc{ fn, module, codegen::compilation_target::module };
    auto const code = cg_prc.declaration_str();  // <-- Generates cpp/raw again
    // Write to .cpp file
}
```

**cpp/raw JIT** (evaluate.cpp):
```cpp
object_ref eval(expr::cpp_raw_ref const expr)
{
    auto const &interpreter = __rt_ctx->jit_prc.interpreter;
    auto parse_res = interpreter->Parse(expr->code.c_str());  // Creates input_line_* symbols
    // ...
}
```

**cpp/raw AOT** (codegen/processor.cpp line 1522):
```cpp
jtl::option<handle> processor::gen(expr::cpp_raw_ref const expr, ...)
{
    util::format_to(deps_buffer, "{}\n", expr->code);  // Just copies code verbatim!
    // NO GUARDS, NO RENAMING, NOTHING
}
```

### Why Existing Guards Don't Exist

Contrary to the problem description mentioning `#ifndef JANK_CPP_RAW_<hash>` guards:
- **These guards don't actually exist in the codebase**
- Searching for `JANK_CPP_RAW` returns zero hits in source code
- The guards were likely planned but never implemented

### Why Guards Wouldn't Help Anyway

Even if we added preprocessor guards:
1. JIT-compiled code is already in LLVM IR/machine code
2. It doesn't have preprocessor directives
3. Guards in AOT C++ won't prevent linking against JIT symbols
4. ODR violations happen at link time, not compile time

## The Solution

### Recommended Fix (Option 1)
**Skip JIT compilation of cpp/raw when in compile mode**

Location: `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/evaluate.cpp`

```cpp
object_ref eval(expr::cpp_raw_ref const expr)
{
#if !defined(JANK_TARGET_WASM) || defined(JANK_HAS_CPPINTEROP)
    // Skip JIT compilation if we're in AOT mode
    if(truthy(__rt_ctx->compile_files_var->deref()))
    {
      return jank_nil;
    }

    // Existing JIT compilation code...
```

**Why this works:**
- When `*compile-files*` is true, cpp/raw isn't JIT compiled
- It only appears in generated C++ source
- No symbol duplication
- REPL mode unchanged (compile-files is false)

### Alternative Considered (Option 2)
Add guards to AOT generated code - but this doesn't solve the root issue of JIT symbols already existing.

## Commands Used

```bash
# Explored the jank codebase
cd /Users/pfeodrippe/dev/jank/compiler+runtime

# Found cpp/raw files
find . -type f -name "*.cpp" -o -name "*.hpp" | grep -E "(context|compile|raw)"

# Searched for cpp/raw handling
grep -r "cpp/raw" src/cpp/jank --include="*.cpp"
grep -r "JANK_CPP_RAW" src/cpp/jank  # Found nothing!

# Examined test case
cat test/bash/ahead-of-time/src/cpp_raw_inline/core.jank

# Checked compilation flow
grep -A 30 "eval(expr::cpp_raw" src/cpp/jank/evaluate.cpp
grep -A 30 "gen(expr::cpp_raw" src/cpp/jank/codegen/processor.cpp
```

## What I'll Do Next

1. ✅ Created comprehensive fix plan at `/Users/pfeodrippe/dev/jank/compiler+runtime/ODR_FIX_PLAN.md`
2. The fix should be a simple one-line check in evaluate.cpp
3. Test with existing `test/bash/ahead-of-time/pass-test`
4. Verify no regressions in REPL mode

## Key Insights

1. **Hybrid Compilation**: jank's standalone mode is hybrid - JIT for execution during load, AOT for final binary
2. **No Double-Work**: We shouldn't JIT compile what we're going to statically compile anyway
3. ***compile-files* Variable**: This is the key flag that differentiates modes
4. **Test Coverage**: The `cpp_raw_inline` test case in ahead-of-time tests exactly this scenario

## Related Code Concepts

- `*compile-files*` dynamic var controls AOT vs JIT-only mode
- `compilation_target` enum distinguishes eval/module/function targets
- `deps_buffer` in codegen processor accumulates top-level definitions
- Expression hashing (analyze/expression_hash.cpp) could be used for guards
- Module loading happens via `context::load_module()` which delegates to module loader

## Files Modified

- Created: `/Users/pfeodrippe/dev/jank/compiler+runtime/ODR_FIX_PLAN.md` (comprehensive fix plan)
- Created: `/Users/pfeodrippe/dev/jank/compiler+runtime/ai/20251212-odr-violation-investigation.md` (this file)
