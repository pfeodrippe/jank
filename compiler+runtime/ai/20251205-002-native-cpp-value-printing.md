# Native C++ Value Printing in REPL

## Problem

When calling C++ functions that return native types (like pointers or structs) that aren't convertible to jank runtime objects, the analyzer would throw an error:

```
This function is returning a native object of type 'ImDrawData *', which is not convertible to a jank runtime object.
```

This was problematic for REPL development when interacting with native C++ code like ImGui.

## Solution

Added support for automatic printing of native C++ values in clang-repl style, e.g.:
- `(TestStruct *) 0x107,2f8,000`
- `(int) 42`

### Implementation Details

1. **New conversion policy** (`include/cpp/jank/analyze/expr/cpp_cast.hpp`):
   - Added `conversion_policy::native_print` enum value
   - This policy is used when we want to wrap a native value in a printable string

2. **Dynamic var** (`clojure.core/*allow-native-return*`):
   - Added `*allow-native-return*` dynamic var (default: false)
   - Tools can use `(binding [*allow-native-return* true] ...)` to enable native printing
   - Declared in `include/cpp/jank/runtime/context.hpp` as `allow_native_return_var`
   - Initialized in `src/cpp/jank/runtime/context.cpp`

3. **Analyzer flag** (`include/cpp/jank/analyze/processor.hpp`):
   - Added `allow_native_return` field (default: false)
   - Can be set directly by C++ code (e.g., in main.cpp for REPL)
   - The analyzer checks BOTH this field AND the dynamic var

4. **Analyzer logic** (`src/cpp/jank/analyze/processor.cpp` ~line 1959):
   - When either `allow_native_return` OR `*allow-native-return*` is truthy
   - AND the return type isn't convertible to jank object
   - Create a `cpp_cast` expression with `native_print` policy
   - Otherwise, throw the existing error

5. **cpp_util ensure_convertible** (`src/cpp/jank/analyze/cpp_util.cpp` ~line 925):
   - Always returns ok() now, allowing all types through at analysis time
   - The runtime check happens in the generated code

6. **Evaluate wrap_expression** (`src/cpp/jank/evaluate.cpp` ~line 210):
   - When wrapping expressions for JIT evaluation
   - For non-convertible types, always uses `native_print` policy
   - The runtime check determines whether to throw or format

7. **Codegen** (`src/cpp/jank/codegen/processor.cpp` ~line 1542):
   - For `native_print` policy, generates code that:
     1. Checks `__rt_ctx->an_prc.allow_native_return` OR `__rt_ctx->allow_native_return_var->deref()` at RUNTIME
     2. If neither is enabled, throws a runtime error with helpful message
     3. If enabled, formats the value as a string using `std::ostringstream`
   - Uses `std::ostringstream` to format: `(Type) address`
   - For pointers, casts to `void const*` to get the address

8. **REPL integration** (`src/cpp/main.cpp` line 263):
   - Set `__rt_ctx->an_prc.allow_native_return = true` for REPL evaluations
   - This means native values are automatically printed when evaluated in the REPL

## Usage

### In the jank REPL (automatic):
```clojure
user=> (cpp/raw "struct TestStruct { int v; }; TestStruct global_test_struct; TestStruct* get_test_ptr() { return &global_test_struct; }")
nil
user=> (cpp/get_test_ptr)
"(TestStruct *) 0x107,2f8,000"
```

### From jank code (for tools):
```clojure
; Enable native return printing for a scope
(binding [*allow-native-return* true]
  (cpp/raw "MyData* ptr = new MyData();")
  ptr)  ; => "(MyData *) 0x..."
```

## Files Modified

- `include/cpp/jank/analyze/expr/cpp_cast.hpp` - Added `native_print` policy
- `include/cpp/jank/analyze/processor.hpp` - Added `allow_native_return` flag
- `include/cpp/jank/runtime/context.hpp` - Added `allow_native_return_var`
- `src/cpp/jank/runtime/context.cpp` - Initialize the dynamic var
- `src/cpp/jank/analyze/processor.cpp` - Check flag and dynamic var, create native_print cast
- `src/cpp/jank/analyze/cpp_util.cpp` - Allow non-convertible types when native return enabled
- `src/cpp/jank/evaluate.cpp` - Handle native_print in wrap_expression
- `src/cpp/jank/codegen/processor.cpp` - Generate string formatting code with ostringstream
- `src/cpp/main.cpp` - Enable flag for REPL

## Test

Test at `test/jank/cpp/native-print/pass-pointer-return.jank`

The test uses `(binding [*allow-native-return* true] ...)` to enable native printing and verifies
that C++ pointers are correctly formatted as strings.

## Key Insights

1. **Runtime check in generated code**: The check for `*allow-native-return*` happens at RUNTIME
   in the generated code, not at analysis time. This allows `(binding [*allow-native-return* true] ...)`
   to work correctly. The generated code checks both `__rt_ctx->an_prc.allow_native_return` AND
   `__rt_ctx->allow_native_return_var->deref()` at runtime.

2. **Always allow through at analysis time**: The `cpp_util::ensure_convertible()` function always
   returns ok() now, allowing non-convertible types through at analysis time. The runtime check
   in the generated code determines whether to throw an error or format the value.

3. **Format escaping issues**: Using nested `jank::util::format()` calls caused escaping problems with `{{}}`. Solution was to use `std::ostringstream` instead.

4. **Lambda capture**: Generated lambdas needed `[&]` capture to access local variables.

5. **Format library behavior**: The jank format library treats `{}` as a placeholder and `{{`/`}}` as literal braces. However, the original codegen for `convert<T>::into_object` used `{}{ ... }` which works because `{` alone (not followed by `}`) is treated as literal.
