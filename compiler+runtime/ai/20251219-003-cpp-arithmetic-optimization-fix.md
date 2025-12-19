# C++ Arithmetic Optimization Fix

## Problem
The Phase 2 arithmetic optimization (converting `dynamic_call` to direct C++ operators) wasn't triggering even when operating on C++ numeric types.

## Root Cause Analysis
1. **`expr_is_cpp_numeric()` returns false for primitive literals** - Their `expression_type()` returns `untyped_object_ptr_type()`
2. **C++ call results are auto-boxed** - When a C++ function call (like `cpp/int.`) returns a value, it gets wrapped in `cpp_cast` with `into_object` policy BEFORE being passed to arithmetic functions like `>`, `<`, `+`, `-`

## Solution

### 1. Look through boxing wrappers (analyze/processor.cpp)
Added helper to unwrap `cpp_cast` with `into_object` policy to find underlying C++ types:

```cpp
auto const get_underlying_expr = [](expression_ref const &arg) -> expression_ref {
  if(arg->kind == expression_kind::cpp_cast)
  {
    auto const cast{ static_cast<expr::cpp_cast *>(arg.data) };
    if(cast->policy == conversion_policy::into_object)
    {
      return cast->value_expr;
    }
  }
  return arg;
};
```

### 2. Accept primitive numeric literals
Allow primitive literals with numeric data (integer/real) as compatible operands:

```cpp
auto const is_primitive_numeric_literal = [](expression_ref const &expr) -> bool {
  if(expr->kind != expression_kind::primitive_literal)
  { return false; }
  auto const lit{ static_cast<expr::primitive_literal *>(expr.data) };
  return lit->data->type == runtime::object_type::integer
         || lit->data->type == runtime::object_type::real;
};
```

### 3. Require at least one C++ type
Don't optimize pure jank arithmetic like `(+ 1 2)` - require at least one actual C++ type:

```cpp
bool has_cpp_numeric{ false };
bool all_compatible{ true };

for(auto const &arg : arg_exprs)
{
  auto const underlying{ get_underlying_expr(arg) };
  if(cpp_util::expr_is_cpp_numeric(underlying))
  { has_cpp_numeric = true; }
  else if(!is_primitive_numeric_literal(underlying))
  {
    all_compatible = false;
    break;
  }
}

bool all_numeric{ all_compatible && has_cpp_numeric };
```

### 4. Emit raw values for primitive literals (codegen/processor.cpp)
Updated operator codegen to emit raw values instead of boxed constant refs:

```cpp
auto const get_operand_str = [&](expression_ref const &arg_expr) -> jtl::immutable_string {
  if(arg_expr->kind == analyze::expression_kind::primitive_literal)
  {
    auto const lit{ static_cast<analyze::expr::primitive_literal *>(arg_expr.data) };
    if(lit->data->type == runtime::object_type::integer)
    {
      auto const i{ runtime::expect_object<runtime::obj::integer>(lit->data) };
      return util::format("{}LL", i->data);
    }
    else if(lit->data->type == runtime::object_type::real)
    {
      auto const r{ runtime::expect_object<runtime::obj::real>(lit->data) };
      return util::format("{}", r->data);
    }
  }
  return gen(arg_expr, arity).unwrap().str(false);
};
```

## Result
Before (form-cpp.txt):
```cpp
jank::runtime::dynamic_call(user__GT__17->deref(), boxed_val, user_const_18);
```

After:
```cpp
auto &&user_cpp_operator_24(user_x_16 > 0LL);
```

## Testing
Verified with:
```clojure
(require 'jank.compiler)
(jank.compiler/native-source '(let [x (cpp/int. 5)] (> x 0)))
```

Test suite: 228 passed, 1 failed (same as baseline)
