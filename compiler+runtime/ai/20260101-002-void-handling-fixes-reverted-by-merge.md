# Void Handling Fixes Reverted by Merge

**Date:** 2026-01-01
**Issue:** origin/main merge reverted void handling fixes
**Test Failures:** 2 tests (pass-void-in-if-branches.jank, pass-void-in-let-body.jank)

## Background

The merge of origin/main into nrepl-4 branch (commit c7c74a0eb) reverted critical void handling fixes that were previously implemented in commits:
- 3118ff59a "Fix if void -> nil"
- 97fed5973 "Return void as nil"

## Root Cause

The merge conflict resolution accidentally reverted the following changes in `src/cpp/jank/codegen/processor.cpp`:

### 1. If Expression Type Inference (Line ~1663)

**Reverted FROM (correct):**
```cpp
auto const expr_type{ cpp_util::non_void_expression_type(expr->then) };
```

**Reverted TO (broken):**
```cpp
auto const expr_type{ cpp_util::expression_type(expr->then) };
```

**Impact:** If expressions with void-returning functions in branches failed with "variable has incomplete type 'void'" error.

### 2. Let Expression Type Inference (Line ~1418)

**Reverted FROM (correct):**
```cpp
auto const last_expr_type{ cpp_util::non_void_expression_type(
  expr->body->values[expr->body->values.size() - 1]) };
```

**Reverted TO (broken):**
```cpp
auto const last_expr_type{ cpp_util::expression_type(
  expr->body->values[expr->body->values.size() - 1]) };
```

### 3. Letfn Expression Type Inference (Line ~1533)

Same issue as let - `expression_type` reverted instead of `non_void_expression_type`.

### 4. C++ Global Function Call Void Handling (Lines ~1964-2005)

**Reverted FROM (correct):**
```cpp
auto const is_void{ Cpp::IsVoid(Cpp::GetFunctionReturnType(source->scope)) };

if(!is_void)
{
  util::format_to(body_buffer, "auto &&{}{ ", ret_tmp);
}

util::format_to(body_buffer, "{}(", Cpp::GetQualifiedCompleteName(source->scope));
// ... args ...
util::format_to(body_buffer, ")");

if(!is_void)
{
  util::format_to(body_buffer, "};");
}
else
{
  /* For void-returning functions, call the function first, then set return temp to nil. */
  util::format_to(body_buffer,
                  ";jank::runtime::object_ref const {}{ jank::runtime::jank_nil };",
                  ret_tmp);
}
```

**Reverted TO (broken):**
```cpp
if(is_void)
{
  util::format_to(body_buffer, "jank::runtime::object_ref const {};", ret_tmp);
}
else
{
  util::format_to(body_buffer, "auto &&{}{ ", ret_tmp);
}
// ... function call ...
else
{
  util::format_to(body_buffer, ";");
}
```

**Issue:** Old code declared the variable BEFORE the void function call, leaving it uninitialized. New code calls the function first, THEN assigns `jank_nil` to the return variable.

### 5. C++ Member Function Call Void Handling (Lines ~2171-2214)

Similar issue as #4 - variable declared before void function call instead of after.

## Fixes Applied

Re-applied all 5 changes from the original commits:

1. **Line 1663:** Changed `expression_type` → `non_void_expression_type` for if expressions
2. **Line 1418:** Changed `expression_type` → `non_void_expression_type` for let expressions
3. **Line 1533:** Changed `expression_type` → `non_void_expression_type` for letfn expressions
4. **Lines 1964-2005:** Fixed global function call void handling to set return value to `jank_nil` after function call
5. **Lines 2171-2214:** Fixed member function call void handling to set return value to `jank_nil` after function call

## Test Cases Fixed

- `test/jank/cpp/call/global/pass-void-in-if-branches.jank` - Tests if expressions with void calls in both branches
- `test/jank/cpp/call/global/pass-void-in-let-body.jank` - Tests let expressions returning void function calls

## Key Learning

**When tests have "pass-" prefix, they WERE passing before!** If they fail after a merge, the merge likely reverted working fixes. Always check recent commits that touched the relevant functionality.

## Critical Fix: jank_nil() Must Be Called As Function

**IMPORTANT**: After re-applying the fixes, discovered that `jank_nil` must be called with parentheses:

```cpp
// WRONG - jank_nil is a function, not a value
jank::runtime::object_ref const ret{ jank::runtime::jank_nil };

// CORRECT - must call the function
jank::runtime::object_ref const ret{ jank::runtime::jank_nil() };
```

**Error if not called**: `no matching constructor for initialization of 'object_ref'` because `jank_nil` has function type `obj::nil_ref ()`, not value type.

This applies to all void handling locations:
- Let/letfn variable initialization: `jank_nil()` not `jank_nil`
- cpp_call void handling (all 3 paths): `jank_nil()` not `jank_nil`

## Final Test Results

After applying all fixes with proper `jank_nil()` function calls:

**jank file tests**: 615 files tested, 12 skips, **0 failures** ✅
- Previously: 3 failures (pass-void-in-if-branches.jank, pass-void-in-let-body.jank, native-header-functions/pass-basic.jank)
- All void-related tests now passing!

**C++ unit tests**: 271 test cases, 267 passed, 4 failed
- Previously: 11 failed
- Remaining 4 failures are nREPL-related, not void handling:
  - test/cpp/jank/nrepl/eval.cpp:137
  - test/cpp/jank/nrepl/eval.cpp:321
  - test/cpp/jank/nrepl/info.cpp:700
  - test/cpp/jank/nrepl/native.cpp:798

## Related Documentation

See also:
- `ai/20251213-void-returns-nil-fix.md` - Original void handling implementation
- Git commits: 3118ff59a, 97fed5973
