# Void-returning C++ functions now return nil

## Problem

When calling void-returning C++ functions from jank, users had to manually add `nil` after the call to avoid compilation errors:

```clojure
;; Before fix - required explicit nil
(defn dispatch-compute! [cmd]
  (let [vk-cmd (cpp/unbox ...)]
    (vkCmdBindPipeline vk-cmd ...)
    nil))  ;; <-- Required or compilation would fail
```

The error was either:
- `no viable overloaded '='` trying to assign to `option<void>`
- Template errors about `cannot form a reference to 'void'`

## Root Cause

Two issues in `/Users/pfeodrippe/dev/jank/compiler+runtime/src/cpp/jank/codegen/processor.cpp`:

1. **cpp_call codegen**: Created uninitialized `object_ref` for void returns
2. **let/letfn codegen**: Used `expression_type` which returns void, causing `option<void>` template errors

## Fix (Smart Approach)

Instead of fixing case-by-case, we use the existing `non_void_expression_type` function which converts void to `untyped_object_ptr_type()` (object_ref).

### Change 1: let/letfn codegen (lines 1085, 1208)
```cpp
// Before
auto const last_expr_type{ cpp_util::expression_type(
  expr->body->values[expr->body->values.size() - 1]) };

// After - void becomes object_ref type
auto const last_expr_type{ cpp_util::non_void_expression_type(
  expr->body->values[expr->body->values.size() - 1]) };
```

### Change 2: cpp_call codegen (lines ~1670, ~1725)
```cpp
// Before - uninitialized object_ref
if(is_void)
{
  util::format_to(body_buffer, "jank::runtime::object_ref const {};", ret_tmp);
}

// After - call function, then assign nil
else
{
  util::format_to(body_buffer,
                  ";jank::runtime::object_ref const {}{ jank::runtime::jank_nil };",
                  ret_tmp);
}
```

## Key Insight

jank already has `non_void_expression_type()` in `cpp_util.cpp` (line 773) that handles the void→object conversion. The comment says:
> "Void is a special case which gets turned into nil, but only in some circumstances."

We just needed to use it in more places!

## New Test

Added `test/jank/cpp/call/global/pass-void-in-let-body.jank` - tests that a let returning a void call returns nil.

## Commands

```bash
cd /Users/pfeodrippe/dev/jank/compiler+runtime
export SDKROOT=$(xcrun --show-sdk-path)
export CC=$PWD/build/llvm-install/usr/local/bin/clang
export CXX=$PWD/build/llvm-install/usr/local/bin/clang++
./bin/compile
./build/jank run test/jank/cpp/call/global/pass-void-in-let-body.jank  # Returns :success
./build/jank run test/jank/cpp/call/global/pass-void.jank  # Returns :success
```

## What's Next

- Void C++ functions now seamlessly integrate everywhere (let, def, etc.)
- Users no longer need workaround `nil` after void calls
- The `non_void_expression_type` pattern can be applied to other places if needed
