# Fix for Issue #582: ODR Violation for cpp/raw Functions During AOT Compilation

## Problem

When compiling a jank module containing `cpp/raw` expressions with inline C++ function definitions, 
the compiler would throw ODR (One Definition Rule) violation errors:

```
error: redefinition of 'hello'
```

This occurred because the same `cpp/raw` block was being embedded in the generated code multiple 
times—once for each function in the module that was compiled.

### Root Cause

In the compilation process:
1. Each top-level form in a module is wrapped in a function and analyzed
2. Each function gets its own codegen processor instance
3. When a `cpp/raw` expression is encountered, its C++ code is collected into the processor's `deps_buffer`
4. When the function's code is generated, the `deps_buffer` contents are prepended to the function definition
5. When multiple functions are compiled in the same module, the same `cpp/raw` code appears multiple times
6. During linking or final compilation, the C++ compiler sees duplicate definitions and throws an ODR error

## Solution

Wrap each `cpp/raw` block in C++ preprocessor include guards based on a hash of its content:

```cpp
#ifndef JANK_CPP_RAW_<hash>
#define JANK_CPP_RAW_<hash>
<cpp/raw code>
#endif
```

When the same `cpp/raw` code appears multiple times:
- All instances have the same hash
- The first inclusion succeeds
- Subsequent inclusions are skipped by the `#ifndef` guard
- No ODR violation occurs

## Implementation

### Changes to `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

Modified the `processor::gen(expr::cpp_raw_ref const ...)` method to wrap cpp/raw code in include guards.

**Before:**
```cpp
util::format_to(deps_buffer, "{}", expr->code);
```

**After:**
```cpp
auto const code_hash{ expr->code.to_hash() };
auto const guard_name{ util::format("JANK_CPP_RAW_{:x}", code_hash) };

util::format_to(deps_buffer, "#ifndef {}\n", guard_name);
util::format_to(deps_buffer, "#define {}\n", guard_name);
util::format_to(deps_buffer, "{}\n", expr->code);
util::format_to(deps_buffer, "#endif\n");
```

### Changes to `compiler+runtime/src/cpp/jank/codegen/llvm_processor.cpp`

Applied the same fix to the JIT compilation path for consistency, though the JIT path through 
the Clang interpreter may be more tolerant of redefinitions. The guard still prevents potential issues.

## Testing

Created test cases to verify the fix:

1. `compiler+runtime/test/bash/module/cpp-raw-simple/pass-test` — Simple case with single cpp/raw block
2. `compiler+runtime/test/bash/module/cpp-raw-dedup/pass-test` — Complex case with multiple cpp/raw blocks, 
   including duplicates

Both tests compile a module with cpp/raw inline function definitions and verify no ODR errors occur.

## Backward Compatibility

This change is fully backward compatible:
- Only affects the internal code generation, not the jank language semantics
- Transparent to users; no API changes
- Existing code with cpp/raw will continue to work as before, just without ODR errors

## Edge Cases Handled

1. **Empty cpp/raw blocks**: Hash is still computed; `#ifndef` guard is generated and works correctly
2. **Identical cpp/raw blocks**: All instances have the same guard name and hash; only first is included
3. **Hash collisions**: Extremely unlikely with a quality hash function; immutable_string.to_hash() is robust
4. **cpp/raw with macros**: Preprocessor directives work fine within the guarded block
