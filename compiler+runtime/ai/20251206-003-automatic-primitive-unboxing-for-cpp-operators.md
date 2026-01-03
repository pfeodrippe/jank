# Automatic Primitive Unboxing for C++ Operators

## Summary

Implemented automatic unboxing of primitive literals when used with C++ builtin operators (`cpp/+`, `cpp/-`, `cpp/*`, `cpp//`, etc.). This allows code like:

```clojure
(let [n 5 m 10]
  (cpp/+ n m))  ; Now works without (cpp/int. 5) etc.
```

## Problem

Previously, using primitive literals in let bindings and then passing them to cpp/ operators would fail because:
1. Primitive literals have boxed types (`object_ref`) at the expression level
2. cpp/ operators expect native C++ types like `long` or `double`
3. Users had to manually construct C++ types: `(cpp/+ (cpp/int. n) (cpp/int. m))`

## Solution

Added automatic detection and conversion of primitive literals in `build_builtin_operator_call()` in `compiler+runtime/src/cpp/jank/analyze/processor.cpp`.

### Changes Made

1. **New helper function `get_primitive_native_type()`** (line ~465):
   - Detects if an expression is a primitive literal (directly or via local_reference)
   - Returns the appropriate native C++ type (`long` for integers, `double` for reals)
   - Returns nullptr if not a primitive or not numeric

2. **Modified `build_builtin_operator_call()`** (line ~514):
   - Before validating operator arguments, check each argument
   - If argument is a boxed object but refers to a primitive literal:
     - Wrap it in a `cpp_cast` expression with `conversion_policy::from_object`
     - Update the argument type to the native type
   - This allows the operator validation and generation to proceed with native types

### Key Code

```cpp
static jtl::ptr<void> get_primitive_native_type(expression_ref const expr)
{
  runtime::object_type data_type{};

  if(expr->kind == expression_kind::primitive_literal)
  {
    auto const lit = llvm::cast<expr::primitive_literal>(expr.data);
    data_type = lit->data->type;
  }
  else if(expr->kind == expression_kind::local_reference)
  {
    auto const ref = llvm::cast<expr::local_reference>(expr.data);
    if(ref->binding->value_expr.is_some())
    {
      auto const val_expr = ref->binding->value_expr.unwrap();
      if(val_expr->kind == expression_kind::primitive_literal)
      {
        auto const lit = llvm::cast<expr::primitive_literal>(val_expr.data);
        data_type = lit->data->type;
      }
      // ...
    }
  }

  switch(data_type)
  {
    case runtime::object_type::integer:
      return Cpp::GetType("long");
    case runtime::object_type::real:
      return Cpp::GetType("double");
    default:
      return nullptr;
  }
}
```

### Generated Code Example

For `(let [n 5 m 10] (cpp/+ n m))`:

**Before:** Would fail with type error - object_ref cannot be used with `+`

**After:** Generates:
```cpp
auto const n_cast{ jank::runtime::convert<long>::from_object(n) };
auto const m_cast{ jank::runtime::convert<long>::from_object(m) };
auto result( n_cast + m_cast );
```

## Supported Operations

This works with all cpp/ builtin operators:
- Arithmetic: `cpp/+`, `cpp/-`, `cpp/*`, `cpp//`, `cpp/%`
- Comparison: `cpp/<`, `cpp/>`, `cpp/<=`, `cpp/>=`, `cpp/==`, `cpp/!=`
- Bitwise: `cpp/&`, `cpp/|`, `cpp/^`, `cpp/~`, `cpp/<<`, `cpp/>>`
- Logical: `cpp/&&`, `cpp/||`, `cpp/!`
- Increment/Decrement: `cpp/++`, `cpp/--`
- Array access: `cpp/aget` (index argument is auto-unboxed)

## Bug Fix: Overloaded Operators (ImVector, etc.)

The initial implementation only applied auto-unboxing in `build_builtin_operator_call()`, which handles builtin operators on primitive types (pointers, arrays, integers). However, for class types with overloaded `operator[]` (like ImGui's `ImVector`), the code takes a different path through `Cpp::GetOperator()`.

**Problem**: `(cpp/aget ImVector n)` where `n` is a primitive literal would fail:
```
No matching call to 'aget' function. With argument 0 having type 'ImVector<ImDrawList *> &'.
With argument 1 having type 'jank::runtime::object *'.
```

**Fix**: Added auto-unboxing logic in `build_cpp_call()` (line ~754) before the `Cpp::GetOperator()` call:
```cpp
/* Auto-unbox primitive literals for overloaded operator calls. */
for(usize i{}; i < arg_exprs.size(); ++i)
{
  auto const native_type = get_primitive_native_type(arg_exprs[i]);
  if(native_type && cpp_util::is_any_object(arg_types[i].m_Type))
  {
    // ... wrap in cpp_cast ...
    arg_types[i].m_Type = native_type;
  }
}
```

Now this works:
```clojure
(let [dd (imgui/GetDrawData)]
  (dotimes [n (cpp/.-CmdListsCount dd)]
    (cpp/aget (cpp/.-CmdLists dd) n)))
```

## Limitations

1. Only works for expressions that are directly primitive literals OR local_references to primitive literal bindings
2. Does not work for computed values (e.g., results of function calls)
3. Does not traverse deeper (e.g., let binding to another let binding)

## Tests

All tests pass (591 jank files, 0 failures).

### Test files added:
- `test/jank/cpp/operator/plus/pass-auto-unbox-primitive-literals.jank`
- `test/jank/cpp/operator/aget/pass-auto-unbox-primitive-literals.jank`
- `test/jank/cpp/operator/aget/pass-dotimes-member-access.jank` - Tests dotimes + cpp/aget with pointer arrays
- `test/jank/cpp/operator/aget/pass-overloaded-operator-auto-unbox.jank` - Tests class types with overloaded `operator[]` (like ImVector)

### Test coverage:
```clojure
; Arithmetic operators with primitive literals
(let [n 5 m 10]
  (assert (= 15 (cpp/+ n m)))
  (assert (= 5 (cpp/- m n))))

(let [x 3.14 y 2.0]
  (assert (< 5.0 (cpp/+ x y))))

(let [a 5 b 10]
  (assert (cpp/< a b))
  (assert (cpp/> b a)))

(let [x 6 y 2]
  (assert (= 12 (cpp/* x y)))
  (assert (= 3 (cpp// x y))))

; Array access with auto-unboxed index
(let [n 1]
  (assert (= 200 (cpp/aget arr n))))

; Array access with native-typed C++ member access
(let [idx (cpp/.-count data)]
  (cpp/aget arr idx))

; dotimes iteration variable with cpp/aget
(dotimes [i (cpp/.-count data)]
  (let [v (cpp/aget arr i)]
    ...))

; Class types with overloaded operator[] (like ImVector)
(let [dd (cpp/GetDrawData)]
  (dotimes [n (cpp/.-CmdListsCount dd)]
    (cpp/aget (cpp/.-Items dd) n)))  ; Items is MyVector<int>, a class type
```

### Verified scenarios:
1. Direct primitive literals in let bindings
2. Native-typed variables from C++ member access (already native, no unboxing needed)
3. `dotimes` iteration variables (primitive literal `0` is auto-unboxed)
4. Class types with overloaded `operator[]` (like ImVector) - auto-unboxing applies to the index argument
