# Codegen Performance Improvements - Implementation Plan

**Date**: 2025-12-19
**Prerequisite**: Read `20251219-001-codegen-performance-improvements.md` for problem analysis

## Compiler Architecture Overview

Based on investigation:

```
┌─────────────────────────────────────────────────────────────────┐
│                     COMPILATION PIPELINE                        │
├─────────────────────────────────────────────────────────────────┤
│  read/parse.cpp        → AST (runtime objects)                  │
│  analyze/processor.cpp → Expression tree (analyze::expr::*)     │
│  analyze/pass/*.cpp    → Optimization passes (minimal today)    │
│  codegen/processor.cpp → C++ source code string                 │
└─────────────────────────────────────────────────────────────────┘
```

### Key Files and Their Roles

| File | Role |
|------|------|
| `analyze/processor.cpp` | Main analyzer - creates expression AST |
| `analyze/expression.hpp` | Base `expression` struct with `needs_box` flag |
| `analyze/expr/*.hpp` | Expression types (`call`, `if_`, `cpp_*`, etc.) |
| `analyze/cpp_util.cpp` | `expression_type()`, type checking helpers |
| `analyze/pass/optimize.cpp` | Entry point for optimization passes (minimal) |
| `codegen/processor.cpp` | Generates C++ from expression tree |

### Existing Type Tracking

The compiler already has infrastructure for type tracking:

1. **`needs_box` flag** on expressions - determines if boxing is required
2. **`expression_type()`** in `cpp_util.cpp` - returns C++ type for expressions
3. **`return_tag_type`** on `call` expr - holds return type from `:tag` metadata
4. **`is_primitive()`** - checks if type is a C++ primitive
5. **C++ interop expressions** (`cpp_call`, `cpp_value`, etc.) have explicit `type` field

---

## Implementation Phases

### Phase 1: Enhance Type Tracking (Foundation)

**Goal**: Track C++ types more comprehensively through expressions

#### Step 1.1: Add `cpp_type` Field to `expression` Base Class

**File**: `compiler+runtime/include/cpp/jank/analyze/expression.hpp`

```cpp
struct expression
{
  // ... existing fields ...

  // New: C++ type for this expression (nullptr = jank object type)
  // This enables type-aware optimizations in codegen
  jtl::ptr<void> cpp_type{};
};
```

**Rationale**: Currently, types are computed on-demand via `expression_type()`. Having the type stored directly enables:
- Faster type queries during codegen
- Type inference propagation during analysis
- Cleaner optimization pass logic

#### Step 1.2: Populate `cpp_type` During Analysis

**File**: `compiler+runtime/src/cpp/jank/analyze/processor.cpp`

Modify expression construction sites to set `cpp_type`:

- `analyze_cpp_call`: Set from `Cpp::GetFunctionReturnType()`
- `analyze_cpp_member_access`: Set from member type
- `analyze_cpp_constructor_call`: Set from constructor return type
- `analyze_primitive_literal`: Set based on literal type (int → `i64`, float → `f64`)
- `analyze_let`: Propagate type from binding value expression

#### Step 1.3: Create Type Query Helpers

**File**: `compiler+runtime/include/cpp/jank/analyze/cpp_util.hpp`

```cpp
namespace cpp_util {
  // Returns true if expression has known C++ primitive type
  bool is_cpp_primitive(expression_ref expr);

  // Returns true if expression is statically known C++ int type
  bool is_cpp_integer(expression_ref expr);

  // Returns true if expression is statically known C++ bool type
  bool is_cpp_bool(expression_ref expr);

  // Returns true if expression is statically known C++ float type
  bool is_cpp_float(expression_ref expr);

  // Gets the C++ type, falling back to object_ref if not a C++ type
  jtl::ptr<void> get_cpp_type_or_object(expression_ref expr);
}
```

---

### Phase 2: Primitive Arithmetic Optimization

**Goal**: `(+ a b)` where a, b are C++ ints → `a + b` instead of dynamic_call

#### Step 2.1: Detect Optimizable Core Calls in Analyzer

**File**: `compiler+runtime/src/cpp/jank/analyze/processor.cpp`

In `analyze_call`, when the callee is a `clojure.core` arithmetic function:

```cpp
// In analyze_call, after macro expansion check:
if(var_deref && var_deref->qualified_name->ns == "clojure.core")
{
  auto const &fn_name = var_deref->qualified_name->name;

  // Check if this is optimizable arithmetic
  if(fn_name == "+" || fn_name == "-" || fn_name == "*" || fn_name == "/")
  {
    // Analyze arguments first
    auto args = analyze_args(...);

    // Check if all args have C++ numeric types
    if(all_cpp_numeric(args))
    {
      // Convert to cpp_builtin_operator_call instead of call
      return build_builtin_operator_call(operator_for(fn_name), args, ...);
    }
  }

  // Similarly for <, >, <=, >=, =
  if(fn_name == "<" || fn_name == ">" || ...)
  {
    if(all_cpp_comparable(args))
    {
      return build_builtin_operator_call(...);
    }
  }
}
```

**Key insight**: `build_builtin_operator_call` already exists for C++ operator expressions. We just need to route core arithmetic/comparison calls through it when types permit.

#### Step 2.2: Extend `cpp_builtin_operator_call` for Core Functions

**File**: `compiler+runtime/include/cpp/jank/analyze/expr/cpp_builtin_operator_call.hpp`

The existing `cpp_builtin_operator_call` already handles operators. We may need to:
- Add support for integer division vs floating division
- Handle mixed int/float promotion rules

---

### Phase 3: Boolean Expression Optimization

**Goal**: `(and a b c)` where all are C++ bools → `a && b && c`

#### Step 3.1: Recognize `and`/`or` in Analyzer

**File**: `compiler+runtime/src/cpp/jank/analyze/processor.cpp`

Currently, `and`/`or` are macros that expand to nested `if`. The analyzer sees the expanded form.

**Option A**: Pre-expansion detection
- Check for `and`/`or` symbols before macroexpand
- If all arguments have C++ bool types, synthesize `cpp_builtin_operator_call` with `&&`/`||`

**Option B**: Post-expansion optimization pass
- Add a pass that recognizes the nested-if pattern from `and`/`or` expansion
- Transform back to `&&`/`||` when all branches are C++ bools

**Recommendation**: Option A is simpler. Add detection before macroexpand:

```cpp
// In analyze_call, before macroexpand:
if(sym->name == "and" || sym->name == "or")
{
  // Analyze all args
  auto args = analyze_args_without_box(...);

  if(all_cpp_bool(args))
  {
    return build_short_circuit_expr(sym->name == "and" ? "&&" : "||", args);
  }
  // Otherwise, fall through to macroexpand
}
```

#### Step 3.2: Optimize `not` for C++ Bools

When `(not x)` is called on a C++ bool expression:

```cpp
// In analyze_call for clojure.core/not:
if(arg_count == 1 && is_cpp_bool(args[0]))
{
  return build_builtin_operator_call(unary_not, args, ...);
}
```

#### Step 3.3: Update `if` Codegen for C++ Bool Conditions

**File**: `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

Currently (line 1352):
```cpp
util::format_to(body_buffer, "if(jank::runtime::truthy({})) {", condition_tmp...);
```

When condition is C++ bool:
```cpp
if(cpp_util::is_cpp_bool(expr->condition))
{
  util::format_to(body_buffer, "if({}) {", condition_tmp.unwrap().str(false));
}
else
{
  util::format_to(body_buffer, "if(jank::runtime::truthy({})) {", ...);
}
```

---

### Phase 4: Constant Folding

**Goal**: `(cpp/float. 1.0)` with literal → `1.0f` directly in codegen

#### Step 4.1: Add Constant Detection

**File**: `compiler+runtime/src/cpp/jank/analyze/processor.cpp`

When analyzing `cpp/float.`, `cpp/int.`, `cpp/bool.` with literal arguments:

```cpp
// In analyze_cpp_cast or equivalent:
if(arg is primitive_literal with numeric data)
{
  // Create a cpp_value with known constant value
  auto cpp_val = make_ref<expr::cpp_value>(...);
  cpp_val->is_literal_constant = true;
  cpp_val->literal_value = literal_data;
  return cpp_val;
}
```

#### Step 4.2: Update Codegen for Constant Expressions

**File**: `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

When generating `cpp_constructor_call` with all constant args:

```cpp
// In gen(cpp_constructor_call_ref):
if(all_args_are_literal_constants(expr))
{
  // Emit direct literal construction
  // e.g., ImVec2{10.0f, 10.0f} instead of converting from boxed
  emit_literal_constructor(expr);
}
```

---

### Phase 5: Statement Position Optimization

**Goal**: Don't generate unused result bindings for void-returning expressions

#### Step 5.1: Track Void Returns in cpp_call

**File**: `compiler+runtime/include/cpp/jank/analyze/expr/cpp_call.hpp`

Add:
```cpp
bool returns_void{false};
```

Set during analysis based on `Cpp::GetFunctionReturnType()`.

#### Step 5.2: Skip Result Binding in Codegen

**File**: `compiler+runtime/src/cpp/jank/codegen/processor.cpp`

In `gen(cpp_call_ref)`:

```cpp
if(expr->position == expression_position::statement && expr->returns_void)
{
  // Just emit the call, no result binding
  emit_call_without_result(expr);
  return none;
}
```

---

### Phase 6: Core Function Inlining

**Goal**: `(count objs)` when `objs` type is known → direct `.size()` call

#### Step 6.1: Build Inline Function Registry

**File**: New file `compiler+runtime/include/cpp/jank/analyze/inline.hpp`

```cpp
namespace analyze::inline_fns {
  // Returns optimized expression if function can be inlined
  // Returns none if no optimization possible
  jtl::option<expression_ref> try_inline(
    obj::symbol_ref fn_name,
    native_vector<expression_ref> const& args,
    expression_position position,
    local_frame_ptr frame);
}
```

#### Step 6.2: Implement Common Inlines

```cpp
// count on vector → .size()
if(fn_name == "count" && is_vector_type(args[0]))
{
  return make_ref<expr::cpp_member_call>(
    position, frame, needs_box,
    args[0], "size", {});
}

// first on vector → [0] (with bounds check)
// nth on vector → operator[]
// get on keyword + map → direct lookup
```

#### Step 6.3: Integrate into analyze_call

In `analyze_call`, before creating `call` expression:

```cpp
if(var_deref)
{
  auto inlined = inline_fns::try_inline(
    var_deref->qualified_name, arg_exprs, position, current_frame);
  if(inlined.is_some())
  {
    return inlined.unwrap();
  }
}
```

---

### Phase 7: Optimization Pass Infrastructure

**Goal**: Enable multi-pass optimizations on the AST

#### Step 7.1: Extend Optimize Entry Point

**File**: `compiler+runtime/src/cpp/jank/analyze/pass/optimize.cpp`

```cpp
expression_ref optimize(expression_ref expr)
{
  profile::timer const timer{ "optimize ast" };

  expr = strip_source_meta(expr);

  // New passes
  expr = fold_constants(expr);      // Phase 4
  expr = simplify_bool_ops(expr);   // Phase 3
  expr = inline_core_calls(expr);   // Phase 6
  expr = eliminate_dead_code(expr); // Phase 5

  return expr;
}
```

#### Step 7.2: Create Pass Template

Each pass follows the recursive visitor pattern already in `analyze/visit.hpp`:

```cpp
// Example: constant folding pass
expression_ref fold_constants(expression_ref expr)
{
  return analyze::walk(expr, [](expression_ref e) -> expression_ref {
    // Transform e if it's a foldable constant operation
    if(is_constant_binary_op(e))
    {
      return evaluate_constant_binary_op(e);
    }
    return e;
  });
}
```

---

## Testing Strategy

### Unit Tests

For each phase, add tests in `compiler+runtime/test/cpp/jank/analyze/`:

1. **Type tracking tests**: Verify `cpp_type` is correctly set
2. **Arithmetic optimization tests**: Verify `(+ int int)` becomes operator
3. **Boolean optimization tests**: Verify `(and bool bool)` becomes `&&`
4. **Constant folding tests**: Verify `(cpp/float. 1.0)` becomes literal

### Integration Tests

Add jank source files that exercise the optimizations:

```clojure
;; test/jank/optimizations/primitive_math.jank
(defn test-int-add []
  (let [a (cpp/int. 5)
        b (cpp/int. 3)]
    ;; Should compile to: a + b
    (+ a b)))
```

### Benchmark Tests

Compare performance before/after optimizations using the same form-jank.txt example.

---

## Implementation Order

Recommended order based on impact and dependencies:

1. **Phase 1** (Type Tracking) - Foundation for everything else
2. **Phase 3.3** (if codegen for bool) - Simple, high impact
3. **Phase 5** (Statement position) - Simple, reduces noise
4. **Phase 4** (Constant folding) - Medium complexity, high impact
5. **Phase 2** (Primitive arithmetic) - Medium complexity, very high impact
6. **Phase 3** (Boolean operators) - Medium complexity, high impact
7. **Phase 6** (Core inlining) - Complex, good payoff
8. **Phase 7** (Pass infrastructure) - Enables future optimizations

---

## Estimated Complexity

| Phase | Files Changed | New Code (est.) | Complexity |
|-------|---------------|-----------------|------------|
| 1 | 3 | ~150 lines | Medium |
| 2 | 2 | ~100 lines | Medium |
| 3 | 2 | ~150 lines | Medium |
| 4 | 2 | ~100 lines | Low |
| 5 | 2 | ~50 lines | Low |
| 6 | 3 | ~200 lines | High |
| 7 | 3 | ~100 lines | Medium |

**Total**: ~850 lines of new/modified code

---

## Risks and Mitigations

### Risk 1: Type Inference Correctness
**Mitigation**: Start with explicit C++ interop types only. Don't infer types for pure jank expressions initially.

### Risk 2: Breaking Semantics
**Mitigation**: Extensive testing. Keep optimizations behind a flag initially (`--optimize-codegen`).

### Risk 3: Compilation Time
**Mitigation**: Profile the optimization passes. Keep them simple and O(n).

### Risk 4: Edge Cases in Type Coercion
**Mitigation**: C++ has complex promotion rules. Defer to standard C++ semantics; don't try to replicate.

---

## Future Work (Not in This Plan)

- **Escape analysis** for stack allocation of short-lived objects
- **Loop unrolling** for small fixed iterations
- **Dead code elimination** beyond unused results
- **Common subexpression elimination**
- **Register allocation hints** for the C++ compiler

---

## Implementation Results (2025-12-19)

### What Was Implemented

#### Phase 1: Type Tracking Infrastructure ✅
Added to `cpp_util.hpp/cpp_util.cpp`:
- `is_boolean_type(type)` - checks if type is C++ bool
- `is_integer_type(type)` - checks if type is C++ integer (int, long, etc.)
- `is_floating_type(type)` - checks if type is C++ float/double
- `is_numeric_type(type)` - combined integer or floating check
- `is_void_type(type)` - checks if type is void
- `expr_is_cpp_bool(expr)` - checks if expression produces C++ bool
- `expr_is_cpp_numeric(expr)` - checks if expression produces C++ numeric
- `expr_is_cpp_primitive(expr)` - combined check

#### Phase 2: Primitive Arithmetic Optimization ✅
In `analyze/processor.cpp::analyze_call()`:
- Routes `clojure.core/+`, `-`, `*`, `/` on C++ numeric types to `cpp_builtin_operator_call`
- Routes `clojure.core/>`, `<`, `>=`, `<=` on C++ numeric types to `cpp_builtin_operator_call`
- Routes `clojure.core/not` on C++ bool to unary `!` operator

#### Phase 3: Boolean Expression Optimization ✅
In `codegen/processor.cpp::gen(if_ref)`:
- When condition is C++ bool, emits `if(cond)` instead of `if(truthy(cond))`

#### Phase 4: Constant Folding for Literals ✅
In `codegen/processor.cpp::gen(cpp_constructor_call_ref)`:
- `(cpp/int. 5)` → `int ret{ static_cast<int>(5) };`
- `(cpp/float. 3.14)` → `float ret{ static_cast<float>(3.14) };`
- Only folds matching types (int literal→int type, float literal→float type)

#### Phase 5: Statement Position Optimization ✅
In `codegen/processor.cpp::gen(cpp_call_ref)`:
- Void-returning C++ calls in statement position emit just `func(args);`
- Eliminates unnecessary `object_ref ret{ nil };` binding

### Performance Impact Analysis

Based on patterns in `form-cpp.txt` (ImGui UI code):

**Before optimization** (pattern from line 14: `(> (sdfx/get_mesh_preview_triangle_count) 0)`):
```cpp
auto &&cpp_call{ sdfx::get_mesh_preview_triangle_count() };
auto const cpp_cast{ jank::runtime::convert<unsigned int>::into_object(cpp_call) };
auto const call(jank::runtime::dynamic_call(>->deref(), cpp_cast, const_0));
```

**After optimization** (Phase 2 routes to C++ operator):
```cpp
auto result{ sdfx::get_mesh_preview_triangle_count() > 0 };
```

**Eliminated per comparison:**
- 1x boxing allocation (`into_object`)
- 1x var deref
- 1x dynamic dispatch
- Multiple function calls

**Before optimization** (pattern: `(cpp/float. 10.0)`):
```cpp
float ret{ jank::runtime::convert<float>::from_object(boxed_10_0) };
```

**After optimization** (Phase 4 constant folding):
```cpp
float ret{ static_cast<float>(10.0) };
```

**Eliminated per literal constructor:**
- 1x `from_object` call (runtime conversion)
- Reference to pre-boxed constant

### Patterns Improved in form-cpp.txt

| Pattern | Count | Optimization |
|---------|-------|--------------|
| `dynamic_call(>->deref(), ...)` | 1 | Phase 2: direct `>` |
| `dynamic_call(<->deref(), ...)` | 3 | Phase 2: direct `<` |
| `dynamic_call(+->deref(), ...)` | 1 | Phase 2: direct `+` |
| `dynamic_call(-->deref(), ...)` | 1 | Phase 2: direct `-` |
| `convert<float>::from_object(...)` | ~30+ | Phase 4: direct literal |
| `truthy(bool_expr)` | multiple | Phase 3: direct bool |
| `object_ref ret{ nil }` for void | multiple | Phase 5: eliminated |

### Test Results
- Baseline: 228 passed, 1 failed
- After all implementations: 228 passed, 1 failed
- **No regressions introduced**

### Future Work (Phases 6-7)
- Phase 6: Core function inlining (`count` → `.size()`, etc.) - deferred
- Phase 7: Multi-pass optimization infrastructure - deferred

These phases require more invasive architectural changes and can be implemented in future iterations.
