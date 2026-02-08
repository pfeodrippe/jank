# eval_string_with_result - Programmatic Access to C++ Interpreter Output

## Problem

Similar to how clang-repl shows expression results like:
```
clang-repl> std::vector<int> L {1,2,3};
clang-repl> L
(std::vector<int> &) @0x1010c0038
```

We wanted programmatic access to the interpreter's printed output from jank.

## Solution

Added `eval_string_with_result()` to `jit::processor` that leverages Clang's `Value` class to capture expression results.

### Key Files Modified

1. **include/cpp/jank/jit/processor.hpp** - Added:
   - `eval_result` struct with fields:
     - `valid` - whether evaluation succeeded
     - `is_void` - whether result is void type
     - `ptr` - raw pointer to result value
     - `type_str` - C++ type as string (e.g., "int", "std::vector<int> &")
     - `repr` - full printed representation (like clang-repl output)
   - `eval_string_with_result()` method declaration

2. **src/cpp/jank/jit/processor.cpp** - Implemented `eval_string_with_result()`:
   - Uses `clang::Value` to capture expression results
   - Calls `ParseAndExecute(code, &value)` with a Value pointer
   - Uses `Value::printType()` and `Value::print()` to capture output to strings
   - Returns `jtl::result<eval_result, jtl::immutable_string>`

3. **test/cpp/jank/jit/processor.cpp** - Added 5 test cases:
   - Integer evaluation (42)
   - Double evaluation (3.14)
   - Void expression (assignment)
   - Pointer evaluation (&var)
   - Error handling (invalid code)

## How clang::Value Works

From `clang/Interpreter/Value.h`:
- `ParseAndExecute(code, &V)` - captures result in Value object
- `V.isValid()` / `V.isVoid()` - check result validity
- `V.getPtr()` - get raw pointer to result
- `V.getType()` - get QualType (Clang's type representation)
- `V.printType(ostream)` - print just the type
- `V.print(ostream)` - print type and value (like clang-repl)

## Usage Example (C++)

```cpp
auto result = __rt_ctx->jit_prc.eval_string_with_result("42");
if (result.is_ok()) {
    auto const& r = result.expect_ok();
    // r.type_str == "int"
    // r.repr contains "(int) 42" or similar
    // r.ptr points to the value in JIT memory
}
```

## Usage Example (jank)

4. **src/cpp/clojure/core_native.cpp** - Added `cpp_eval_with_info()` function:
   - Wraps `eval_string_with_result()` for jank code
   - Returns a jank map with keys: `:valid`, `:void?`, `:ptr`, `:type`, `:repr`
   - Only available in native builds (not WASM)

```clojure
;; From jank code:
(clojure.core-native/cpp-eval-with-info "42")
;; => {:type "int", :ptr 42, :valid true, :repr "(int) 42\n", :void? false}

(clojure.core-native/cpp-eval-with-info "std::vector<int>{1,2,3}")
;; => {:type "std::vector<int>", :ptr 105553164723360, :valid true,
;;     :repr "(std::vector<int>) @0x600002e364a0\n", :void? false}
```

The `:repr` field gives you output like clang-repl, showing type and value/address.
