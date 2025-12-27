# Fix: cpp/value should not be callable when result is not a function

## Issue

The test `fail-integer.jank` containing `((cpp/value "1"))` was incorrectly passing instead of failing.

The test expects `((cpp/value "1"))` to fail because:
1. `(cpp/value "1")` evaluates to the integer literal `1`
2. The outer parentheses `(1)` try to call the integer
3. Integers are not callable, so this should throw an error

## Root Cause

In `analyze_call` (processor.cpp), there was an early return in the "non-symbol head" branch that incorrectly handled `cpp_call` expressions with zero arguments:

```cpp
else if((source->kind >= expression_kind::cpp_value_min
         && source->kind <= expression_kind::cpp_value_max)
        || !cpp_util::is_any_object(cpp_util::expression_type(source.data)))
{
  /* If we have a cpp_call with no additional arguments, just return it.
   * This handles the case where a C macro is used in call position. */
  if(source->kind == expression_kind::cpp_call && arg_count == 0)
  {
    return source.as_ref();  // BUG: This returns the result instead of trying to call it
  }
  return analyze_cpp_call(o, source.data, current_frame, position, fn_ctx, needs_box);
}
```

This early return was intended for C macro symbols in call position (like `(rl/KEY_ESCAPE)`), but was incorrectly applied to the non-symbol head case.

When `(cpp/value "1")` is analyzed, it returns a `cpp_call` expression (because `resolve_literal_value` creates a call to return the literal). The early return then just returned this result, skipping the attempt to call it.

## Fix

Removed the early return in the non-symbol head branch. The early return only makes sense in the symbol head case (lines 3581-3587), where a symbol like `rl/KEY_ESCAPE` directly evaluates to a C macro value.

In the non-symbol head case (when the head is a list like `(cpp/value "1")`), we should always attempt to call the result, which will properly fail for non-callable values.

## Changed File

- `src/cpp/jank/analyze/processor.cpp` (line ~3686): Removed the early return condition

## Test Results

All 581 jank tests pass, including the three not-callable tests:
- `fail-integer.jank` - properly fails when trying to call integer
- `fail-boolean.jank` - properly fails when trying to call boolean
- `fail-nullptr.jank` - properly fails when trying to call nullptr
