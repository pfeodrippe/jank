# Fix for Operator Overloads and cpp/unbox Type Inference After origin/main Merge

**Date:** 2026-01-01
**Issue:** After merging origin/main into nrepl-4 branch, 18 jank file tests failed
**Root Cause:** Two bugs introduced during the merge

## Bug 1: cpp/unbox Type Inference Broken

**Symptom:**
```
error: This call to 'cpp/unbox' is missing a value to unbox as an argument.
```

**Root Cause:**
File: `src/cpp/jank/analyze/processor.cpp` (analyze_cpp_unbox function, lines 5028-5034)

The merge introduced overly strict error checking that rejected single-argument `cpp/unbox` calls before the type inference code could run. The code had:

```cpp
// BUGGY CODE (rejected count == 2)
if(count < 3)
{
  return error::analyze_invalid_cpp_unbox(
    "This call to 'cpp/unbox' is missing a value to unbox as an argument.",
    ...);
}
```

But lines 5046-5106 had working type inference code for `count == 2` (single-argument form).

**Fix:**
Removed the overly restrictive `count < 3` check, allowing type inference to work:

```cpp
// FIXED CODE
if(count < 2)
{
  return error::analyze_invalid_cpp_unbox(
    "This call to 'cpp/unbox' requires at least one argument...",
    ...);
}
else if(3 < count)
{
  return error::analyze_invalid_cpp_unbox(
    "A call to 'cpp/unbox' takes at most two arguments...",
    ...);
}
```

## Bug 2: Operator Overload Errors with Mixed Primitive/Boxed Types

**Symptom:**
```
error: invalid operands to binary expression ('double' and 'const jank::runtime::obj::real_ref')
```

**Root Cause:**
File: `src/cpp/jank/codegen/processor.cpp` (lifted constants generation, line 2524)

ALL lifted constants (like `3.14`) were always generated as boxed types (`real_ref`), even when used in C++ operators that need primitive types. This caused operator overload errors like `double > real_ref`.

**Analysis Phase (lines 4021-4041):**
- Correctly identified that `3.14` is a `primitive_numeric_literal`
- Optimization for cpp operators kicked in
- Created `cpp_builtin_operator_call` expression

**Codegen Phase (lines 2519-2527):**
- ALL lifted constants generated with `boxed = true`
- Result: `jank::runtime::obj::real_ref const const_75035{...}`
- cpp operator tried to use boxed value directly → overload error

**Initial Failed Approach:**
Tried generating inline unboxed constants in `gen(primitive_literal)`, but this caused type errors when those values were assigned to `auto&&` variables and then returned.

**Final Fix:**
Modified `gen(cpp_builtin_operator_call_ref)` to unbox primitive literal arguments on-the-fly when generating operator code:

```cpp
for(auto const &arg_expr : expr->arg_exprs)
{
  auto arg_tmp{ gen(arg_expr, arity).unwrap() };

  /* Unbox primitive literal constants for use in C++ operators */
  if(arg_expr->kind == analyze::expression_kind::primitive_literal)
  {
    auto const lit{ static_cast<analyze::expr::primitive_literal *>(arg_expr.data) };
    if(lit->data->type == runtime::object_type::integer)
    {
      arg_tmp = handle{ util::format(
        "jank::runtime::expect_object<jank::runtime::obj::integer>({})->data",
        arg_tmp.str(false)) };
    }
    else if(lit->data->type == runtime::object_type::real)
    {
      arg_tmp = handle{ util::format(
        "jank::runtime::expect_object<jank::runtime::obj::real>({})->data",
        arg_tmp.str(false)) };
    }
  }

  arg_tmps.emplace_back(arg_tmp);
}
```

## Impact

**Before fixes:** 18 jank file test failures
**After fixes:** All tests passing

**Files Modified:**
1. `src/cpp/jank/analyze/processor.cpp` - Fixed cpp/unbox type inference
2. `src/cpp/jank/codegen/processor.cpp` - Fixed operator overload issues

## Key Learnings

1. **Lifted Constants Are Always Boxed:** The codegen phase boxes all lifted constants regardless of usage context. This is by design (see TODO comment at line 2521).

2. **Analysis vs Codegen:** Type compatibility checks happen during analysis when values are still `primitive_literal` expressions, but codegen later boxes them. Need to handle unboxing at use sites.

3. **Inline Unboxing vs Variable Assignment:** Generating inline unboxed values (like `static_cast<i64>(1)`) works for direct use in expressions, but fails when assigned to `auto&&` variables that are later returned as `object_ref`.

4. **On-the-Fly Unboxing:** The safe approach is to generate boxed constants but unbox them inline at use sites (like in operator expressions) where primitive types are required.

## Test Cases Verified

- `test/jank/cpp/opaque-box/var-type-inference/pass-bool-ptr.jank`
- `test/jank/cpp/opaque-box/var-type-inference/pass-double-ptr.jank`
- `test/jank/cpp/opaque-box/var-type-inference/pass-int-ptr.jank`
- `test/jank/cpp/opaque-box/var-type-inference/pass-long-ptr.jank`
- `test/jank/cpp/opaque-box/var-type-inference/pass-multiple-vars.jank`
- `test/cpp/jank/analyze/box.cpp` (C++ test suite)
- All 18 originally failing jank file tests
