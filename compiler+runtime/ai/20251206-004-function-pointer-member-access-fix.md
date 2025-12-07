# Fix: Calling Function Pointers via Member Access

## Summary

Fixed calling function pointers that are accessed via struct member access (e.g., `(-> cmd cpp/* cpp/.-UserCallback)`). Previously this would fail with "Unable to find call operator for 'Callback &'" because the member access returns a reference to the function pointer, not the function pointer itself.

## Problem

When accessing a function pointer member from a struct, jank returns a reference type:

```clojure
(-> cmd cpp/* cpp/.-UserCallback)  ; Returns `Callback &` (reference to function pointer)
```

The function `build_indirect_cpp_call()` in `processor.cpp` checks `Cpp::IsFunctionPointerType(source_type)`, but this returns `false` for reference types like `Callback &`.

**Error before fix:**
```
Unable to find call operator for 'ImDrawCallback &'.
```

## Solution

Strip the reference type before checking if it's a function pointer. Added in `build_indirect_cpp_call()` at line ~1096:

```cpp
auto source_type{ cpp_util::expression_type(source) };
/* Function pointers accessed via member access (e.g. struct->callback) return
 * a reference to the function pointer type. We need to strip the reference
 * to properly detect and call function pointers. */
if(Cpp::IsReferenceType(source_type))
{
  source_type = Cpp::GetNonReferenceType(source_type);
}
if(!Cpp::IsFunctionPointerType(source_type))
{
  // ... fallback to operator() lookup
}
```

## Usage Example

This enables the ImGui callback pattern:

```clojure
; ImDrawCmd has a UserCallback member which is a function pointer
(let [cmd (get-draw-cmd)]
  (when (-> cmd cpp/* cpp/.-UserCallback)
    ((-> cmd cpp/* cpp/.-UserCallback) draw-list cmd)))
```

## Test Added

`test/jank/cpp/call/function-pointer/pass-member-callback.jank`:
```clojure
(cpp/raw "namespace jank::cpp::call::function_pointer::pass_member_callback
          {
            using Callback = int (*)(int, int);

            struct DrawCmd {
              Callback UserCallback;
              DrawCmd() : UserCallback(nullptr) {}
            };

            int my_callback(int a, int b)
            {
              return a + b;
            }

            DrawCmd* make_cmd()
            {
              static DrawCmd cmd;
              cmd.UserCallback = my_callback;
              return &cmd;
            }
          }")

(let* [cmd (cpp/jank.cpp.call.function_pointer.pass_member_callback.make_cmd)]
  (if (= 142 ((-> cmd cpp/* cpp/.-UserCallback) (cpp/int. 100) (cpp/int. 42)))
    :success))
```

## Tests

All 592 jank tests pass with 0 failures.
