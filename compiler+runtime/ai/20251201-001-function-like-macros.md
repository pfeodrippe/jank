# Function-like Macro Support from C Headers

## Summary

Added support for calling function-like C macros (like `ecs_new_w_pair` from flecs) from jank code using the native alias syntax.

## Key Changes

### 1. New API Functions (`native_header_completion.hpp/cpp`)

Added functions to detect and query function-like macros:

```cpp
bool is_native_header_function_like_macro(native_alias const &alias, std::string const &name);
std::optional<size_t> get_native_header_macro_param_count(native_alias const &alias, std::string const &name);
```

### 2. Macro Enumeration Updated

`enumerate_native_header_macros()` now includes both object-like and function-like macros. Previously it explicitly filtered out function-like macros with:
```cpp
// REMOVED: This filtering was removed
if(md->isFunctionLike()) continue;
```

### 3. Macro Expansion Format

For function-like macros, `get_native_header_macro_expansion()` now returns the full signature:
```
TEST_ADD(a, b) ( ( a ) + ( b ) )
```

Instead of just the body tokens.

### 4. Analyzer Changes (`processor.cpp`)

Added handling for function-like macro calls in `analyze_call()`:

```cpp
if(nrepl_server::asio::is_native_header_function_like_macro(alias_info, member_name))
{
  // Build macro call: "MACRO_NAME(arg1, arg2, ...)"
  // Create (cpp/value "MACRO_CALL(args...)") and analyze it
}
```

Arguments are converted to C++ literals:
- Integer → `std::to_string(i->data)`
- Real → `std::to_string(r->data)`
- Boolean → `"true"` or `"false"`
- Symbol → analyze and extract value or use name directly

### 5. Test Header Additions (`test_c_header.h`)

Added test macros mimicking flecs patterns:
```c
#define TEST_ADD(a, b)        ((a) + (b))
#define TEST_MUL(a, b)        ((a) * (b))
#define TEST_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
```

## Usage Example

```clojure
;; Require header with alias
(require '["flecs.h" :as ecs :scope ""])

;; Call function-like macros
(ecs/ecs_new_w_pair world first second)

;; Nested function calls as macro arguments work too!
(fl/ecs_new_w_pair (fl/ecs_mini) 3 4)

;; From test header
(require '["test_c_header.h" :as th :scope ""])
(th/TEST_ADD 1 2)      ; => 3
(th/TEST_CLAMP 50 0 100) ; => 50

;; Nested zero-arg function calls as macro arguments
(th/TEST_ADD (th/test_get_five) (th/test_get_ten))  ; => 15
```

## Autocompletion

Function-like macros appear in autocompletion results with type "macro":
```
th/TEST_ADD (type: macro)
th/TEST_CLAMP (type: macro)
```

## Tests Added

11 new tests in `test/cpp/jank/nrepl/engine.cpp`:
1. `is_native_header_function_like_macro detects function-like macros`
2. `get_native_header_macro_param_count returns parameter count`
3. `get_native_header_macro_expansion includes parameter signature`
4. `function-like macro invocation via native alias syntax`
5. `function-like macro with nested function call as argument`
6. `function-like macro with local binding to cpp_call as argument`
7. `function-like macro with cpp/box binding as argument`
8. `enumerate_native_header_macros includes function-like macros`
9. `autocompletion includes function-like macros from native alias`
10. `function-like macro with jank expression as argument generates wrapper`
11. `function-like macro with jank arithmetic as argument`

## Nested Function Calls as Macro Arguments

A recursive helper `expr_to_cpp_code` was added to handle nested function calls as macro arguments.
For example, `(fl/ecs_new_w_pair (fl/ecs_mini) 3 4)` generates:
```cpp
ecs_new_w_pair(ecs_mini(), 3, 4)
```

The helper handles:
- `cpp_call` with `function_code` set → use directly
- `cpp_call` without `function_code` → recursively build from `source_expr` and `arg_exprs`
- `cpp_value` → get name from `form` symbol
- `primitive_literal` → convert to string

## Wrapper Function Generation for Jank Expressions

When macro arguments contain jank expressions (local bindings, arithmetic, etc.) that don't directly translate to C++ code, the analyzer generates a wrapper function at JIT compile time.

### How It Works

For example, `(let [x 5] (th/TEST_ADD x 10))` generates:

```cpp
[[gnu::always_inline]] inline jank::runtime::object_ref __jank_macro_wrapper_XXX(
    jank::runtime::object_ref arg0,
    jank::runtime::object_ref arg1) {
  auto val0 = jank::runtime::expect_object<jank::runtime::obj::integer>(arg0)->data;
  auto val1 = jank::runtime::expect_object<jank::runtime::obj::integer>(arg1)->data;
  return jank::runtime::make_box(TEST_ADD(val0, val1));
}
```

The wrapper:
1. Takes `object_ref` parameters for all arguments
2. Unboxes each to `native_integer` using `expect_object<obj::integer>`
3. Calls the macro with the unboxed values
4. Boxes and returns the result via `make_box`

The wrapper is parsed and executed by the JIT interpreter, then a `cpp_call` expression is created to invoke it with the analyzed jank arguments.

### Supported Macro Arguments

All argument types are now supported:
- **Literal values**: `(th/TEST_ADD 1 2)` → 3
- **C++ function calls**: `(th/TEST_ADD (th/test_get_five) (th/test_get_ten))` → 15
- **Local bindings**: `(let [x 5] (th/TEST_ADD x 10))` → 15
- **Jank arithmetic**: `(th/TEST_ADD (+ 3 4) 5)` → 12
- **Boxed C++ pointers**: `(let [a (cpp/box (fl/ecs_mini))] (fl/ecs_new_w_pair a 3 x))`
- **Mixed arguments**: C++ expressions are embedded directly, jank expressions use wrapper

### Type-Aware Unboxing

The wrapper uses type inference to determine the correct unboxing:

1. **Boxed pointers** (`cpp/box` of C++ function call):
   - Detected by tracing `local_reference` → `binding.value_expr` → `cpp_box` → `cpp_call`
   - Unboxes via `native_pointer_wrapper` with cast: `static_cast<T*>(expect_object<native_pointer_wrapper>(arg)->data)`

2. **Integer expressions** (local bindings to primitives, jank arithmetic):
   - Default case when no C++ type is detected
   - Unboxes via `integer`: `expect_object<obj::integer>(arg)->data`

### Mixed Argument Handling

For calls with both C++ and jank arguments, the wrapper:
1. **Embeds C++ expressions directly** (literals, function calls)
2. **Only takes `object_ref` parameters for jank expressions**

Example: `(let [x 5] (fl/ecs_new_w_pair (fl/ecs_mini) 3 x))`
```cpp
[[gnu::always_inline]] inline object_ref wrapper(object_ref arg0) {
  auto val0 = expect_object<obj::integer>(arg0)->data;  // only x
  return make_box(ecs_new_w_pair(ecs_mini(), 3, val0)); // ecs_mini() and 3 embedded
}
```

### Implementation Details (processor.cpp)

Key code in `analyze_call()` for function-like macros:

```cpp
/* Type detection helper - only returns type for cpp_box expressions */
auto get_cpp_type_from_expr = [](expression_ref const &exp) -> option<ptr<void>> {
  if(exp->kind == expression_kind::local_reference) {
    auto const local_ref = llvm::cast<expr::local_reference>(exp.data);
    if(local_ref->binding && local_ref->binding->value_expr.is_some()) {
      auto const val_expr = local_ref->binding->value_expr.unwrap();
      if(val_expr->kind == expression_kind::cpp_box) {
        auto const box = llvm::cast<expr::cpp_box>(val_expr.data);
        if(box->value_expr->kind == expression_kind::cpp_call) {
          return llvm::cast<expr::cpp_call>(box->value_expr.data)->type;
        }
      }
    }
  }
  return none;  // default to integer unboxing
};

/* Type-aware unboxing */
if(maybe_type.is_some() && Cpp::IsPointerType(maybe_type.unwrap())) {
  // Pointer: static_cast<T*>(expect_object<native_pointer_wrapper>(arg)->data)
} else {
  // Integer: expect_object<obj::integer>(arg)->data
}
```

### Current Limitations

- Floating point types not yet supported (would need `obj::real` unboxing)
- Only detects pointer types from direct `cpp/box` expressions, not through complex chains

## Bug Fixes

### Relative Path Matching in Header Detection

Fixed an issue where macros and declarations from C headers weren't being detected when the header path used relative components like `../`.

**Problem**: When including a header with `#include "../test/foo/bar.h"`, the path matching functions `is_macro_from_header` and `is_decl_from_header` would fail because they checked if an absolute path like `/Users/.../test/foo/bar.h` ended with `../test/foo/bar.h`.

**Solution**: Added `get_clean_header_suffix()` helper in `native_header_completion.cpp` that strips leading `../` and `./` path components. Both header matching functions now check against both the original header name and the cleaned suffix.

```cpp
std::string_view get_clean_header_suffix(std::string_view header_name)
{
  while(header_name.starts_with("../") || header_name.starts_with("./"))
  {
    if(header_name.starts_with("../"))
      header_name.remove_prefix(3);
    else if(header_name.starts_with("./"))
      header_name.remove_prefix(2);
  }
  return header_name;
}
```

### Direct cpp_call Binding Detection

Fixed detection of pointer types for local bindings that are direct `cpp_call` results (not wrapped in `cpp/box`).

**Before**: Only `(let [a (cpp/box (fl/ecs_mini))] ...)` was recognized as a pointer type.

**After**: Both forms are now recognized:
- `(let [a (cpp/box (fl/ecs_mini))] ...)`  - boxed pointer
- `(let [a (fl/ecs_mini)] ...)` - direct cpp_call result

The `get_cpp_type_from_expr` helper in `processor.cpp` now handles Case 2 (direct `cpp_call` without boxing).

### Local Bindings to cpp_call, cpp_box, or Primitives Expanded Inline

When a local binding is assigned a `cpp_call`, `cpp_box`, or `primitive_literal` expression, and that binding is used as a macro argument, it now expands inline.

**Examples**:
```clojure
;; Direct cpp_call binding
(let [a (fl/ecs_mini)]
  (fl/ecs_new_w_pair a 3 4))
;; Generates: ecs_new_w_pair(ecs_mini(), 3, 4)

;; cpp/box binding - inner cpp_call is extracted
(let [x 5
      a (cpp/box (fl/ecs_mini))]
  (fl/ecs_new_w_pair a 3 x))
;; Generates: ecs_new_w_pair(ecs_mini(), 3, 5)

;; Primitive literal binding
(let [x 5]
  (th/TEST_ADD x 10))
;; Generates: TEST_ADD(5, 10)
```

**Implementation**: Added `local_reference` handling to `expr_to_cpp_code` in `processor.cpp`:
```cpp
else if(e->kind == expression_kind::local_reference)
{
  auto const local_ref{ llvm::cast<expr::local_reference>(e.data) };
  if(local_ref->binding && local_ref->binding->value_expr.is_some())
  {
    auto const val_expr{ local_ref->binding->value_expr.unwrap() };
    // Direct cpp_call
    if(val_expr->kind == expression_kind::cpp_call)
    {
      return expr_to_cpp_code(val_expr);
    }
    // cpp/box - extract inner expression
    else if(val_expr->kind == expression_kind::cpp_box)
    {
      auto const box{ llvm::cast<expr::cpp_box>(val_expr.data) };
      return expr_to_cpp_code(box->value_expr);
    }
    // Primitive literal
    else if(val_expr->kind == expression_kind::primitive_literal)
    {
      return expr_to_cpp_code(val_expr);
    }
  }
  return jtl::none;
}
```

### Improved C++ Evaluation Error Messages

When C++ code evaluation fails, the error message now includes the failing code (up to 500 characters) for easier debugging.

**Before**: `"Failed to evaluate C++ code.Failed to evaluate C++ code."`

**After**:
```
Failed to evaluate C++ code:
[actual C++ code that failed]...(truncated if >500 chars)
```

**Implementation** (`jit/processor.cpp`):
```cpp
void processor::eval_string(jtl::immutable_string const &s) const
{
  auto err(interpreter->ParseAndExecute({ formatted.data(), formatted.size() }));
  if(err)
  {
    llvm::logAllUnhandledErrors(std::move(err), llvm::errs(), "error: ");
    auto const preview_len{ std::min<size_t>(formatted.size(), 500) };
    native_transient_string code_preview{ formatted.data(), preview_len };
    if(formatted.size() > 500)
    {
      code_preview.append("...(truncated)");
    }
    throw std::runtime_error{
      util::format("Failed to evaluate C++ code:\n{}", code_preview)
    };
  }
}
```

## Clang API Used

- `MacroInfo::isFunctionLike()` - detect macro type
- `MacroInfo::getNumParams()` - get parameter count
- `MacroInfo::params()` - iterate parameter identifiers
