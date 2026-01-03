# Plan: Automatic Type Inference for Local Bindings in `let`

Date: 2025-12-06

## Problem Statement

Currently, when using `cpp/` operators with local bindings, the user must manually handle type conversions:

```clojure
(let [n 5
      m 10]
  ;; This doesn't work directly - n and m are boxed objects
  (cpp/+ n m))  ; ERROR: expects primitive types, gets object*
```

### Goal

Enable automatic type inference so local bindings from primitive literals (and other typed sources) can be used directly with C++ operators:

```clojure
(let [n 5
      m 10]
  ;; n and m should be automatically usable as primitives
  (assert (= 15 (cpp/+ n m)))
  (assert (= 5 (cpp/- m n))))

(let [dd (imgui/GetDrawData)]
  (dotimes [n (cpp/.-CmdListsCount dd)]
    (cpp/aget (cpp/.-CmdLists dd) n)))
```

## Current State Analysis

### What EXISTS

1. **Local binding type tracking** (`local_frame.hpp:61`):
   ```cpp
   struct local_binding {
     jtl::ptr<void> type{ analyze::cpp_util::untyped_object_ptr_type() };
   };
   ```

2. **Type inference in `analyze_let`** (`processor.cpp:~2380`):
   ```cpp
   auto const expr_type{ cpp_util::non_void_expression_type(it.second) };
   ret->frame->locals.emplace(sym,
                              local_binding{ sym, ..., .type = expr_type });
   ```

3. **`expression_type(local_reference)`** returns `binding->type` correctly

4. **Unboxed constant generation** (`codegen/processor.cpp:86-91`):
   ```cpp
   case jank::runtime::object_type::integer:
     if(boxed) return "jank::runtime::obj::integer_ref";
     return "jank::i64";  // Unboxed path exists!
   ```

5. **`cpp_builtin_operator_call`** already works with unboxed args:
   - Uses `arg_tmps[i].str(false)` to get unboxed version
   - Has `type` field for result type

### What's MISSING

1. **Primitive literals don't expose unboxed type** - `primitive_literal` expression type is `object*`

2. **Local bindings default to boxed** - Even though type is tracked, it's always `untyped_object_ptr_type()`

3. **No inference chain** - When `n = 5`, we don't propagate that `n` is actually `i64`

4. **Codegen uses boxed names** - `handle::str()` currently always returns `boxed_name`

## Proposed Solution

### Phase 1: Track Unboxed Types for Primitives

**Goal:** Make `expression_type(primitive_literal)` return the unboxed C++ type for integers, reals, booleans.

**File:** `src/cpp/jank/analyze/cpp_util.cpp`

```cpp
jtl::ptr<void> expression_type(expression_ref const expr) {
  return visit_expr([](auto const typed_expr) -> jtl::ptr<void> {
    // ...existing cases...

    else if constexpr(jtl::is_same<T, expr::primitive_literal>) {
      // NEW: Return actual primitive type based on data
      auto const &data = typed_expr->data;
      switch(data->type) {
        case runtime::object_type::integer:
          return Cpp::GetType("long");  // or jank::i64
        case runtime::object_type::real:
          return Cpp::GetType("double"); // or jank::f64
        case runtime::object_type::boolean:
          return Cpp::GetType("bool");
        default:
          return untyped_object_ptr_type();
      }
    }
  }, expr);
}
```

### Phase 2: Propagate Types to Local Bindings

**Goal:** When `let` analyzes `[n 5]`, store `long` (not `object*`) as `n`'s type.

**File:** `src/cpp/jank/analyze/processor.cpp` (~line 2380)

This already works! The line:
```cpp
auto const expr_type{ cpp_util::non_void_expression_type(it.second) };
```
will pick up the new primitive type from Phase 1.

### Phase 3: Track Unboxed Requirements

**Goal:** Determine if a binding needs boxing based on usage.

**Key insight:** jank already has `needs_box`, `has_boxed_usage`, `has_unboxed_usage` flags on `local_binding`.

**Add a pass** to analyze usage patterns:
- If binding is used with `cpp/+`, `cpp/-`, etc. -> `has_unboxed_usage = true`
- If binding is passed to a jank function -> `has_boxed_usage = true`
- If binding escapes to closure -> `needs_box = true`

**File:** Add new file `src/cpp/jank/analyze/pass/unbox_inference.cpp`

```cpp
void analyze_boxing_requirements(expression_ref root) {
  walk_expressions(root, [](expression_ref expr) {
    if(auto* op = dynamic_cast<cpp_builtin_operator_call*>(expr.get())) {
      for(auto& arg : op->arg_exprs) {
        if(auto* local = dynamic_cast<local_reference*>(arg.get())) {
          local->binding->has_unboxed_usage = true;
        }
      }
    }
    // ... handle other cases
  });
}
```

### Phase 4: Generate Unboxed Local Declarations

**Goal:** Generate unboxed local declarations when possible.

**File:** `src/cpp/jank/codegen/processor.cpp` (`gen(let_ref)`)

Currently generates:
```cpp
{ auto &&n(/* boxed value */); ... }
```

Should generate for primitives:
```cpp
{ jank::i64 n(5); ... }  // Unboxed declaration
```

**Changes:**
```cpp
// In gen(let_ref), around line 1094
if(cpp_util::is_primitive(local_type) && !local->needs_box) {
  // Generate unboxed local
  auto const type_str = cpp_util::get_qualified_type_name(local_type);
  util::format_to(body_buffer, "{ {} {}({}); ",
                  type_str, munged_name, val_tmp.unwrap().str(false));
} else {
  // Existing boxed logic
  util::format_to(body_buffer, "{ auto &&{}({}); ",
                  munged_name, val_tmp.unwrap().str(false));
}
```

### Phase 5: Handle `local_reference` with Correct Boxing

**Goal:** When a local is referenced, use unboxed name if available.

**File:** `src/cpp/jank/codegen/processor.cpp` (`gen(local_reference_ref)`)

The `handle` class already supports this! Just need to ensure proper boxing info flows through:

```cpp
jtl::option<handle> processor::gen(analyze::expr::local_reference_ref const expr, ...) {
  handle ret;
  if(expr->binding->needs_box) {
    ret = munged_name;  // Boxed
  } else {
    ret = handle{ "", munged_name };  // Unboxed with auto-boxing if needed
  }
  // ...
}
```

### Phase 6: Re-enable `handle::str(needs_box)` Logic

**File:** `src/cpp/jank/codegen/processor.cpp` (lines 480-495)

Currently commented out:
```cpp
jtl::immutable_string handle::str([[maybe_unused]] bool const needs_box) const {
  return boxed_name;  // Always returns boxed!
  // ... commented out logic for unboxed ...
}
```

Uncomment and enable:
```cpp
jtl::immutable_string handle::str(bool const needs_box) const {
  if(needs_box) {
    if(boxed_name.empty()) {
      throw std::runtime_error{...};
    }
    return boxed_name;
  } else {
    return unboxed_name;
  }
}
```

## Implementation Order

1. **Phase 1** - Make `expression_type()` return primitive types for literals
2. **Phase 2** - Verify type propagation works in `analyze_let`
3. **Phase 6** - Re-enable `handle::str()` unboxed logic
4. **Phase 4** - Generate unboxed local declarations
5. **Phase 5** - Ensure `local_reference` codegen uses unboxed correctly
6. **Phase 3** - Add boxing requirement analysis pass (for complex cases)

## Edge Cases to Handle

### 1. Mixed Boxing Contexts
```clojure
(let [n 5]
  (println n)      ; Needs boxed
  (cpp/+ n 10))    ; Needs unboxed
```
**Solution:** Generate both boxed and unboxed versions when `has_boxed_usage && has_unboxed_usage`

### 2. Closure Capture
```clojure
(let [n 5]
  (fn [] n))  ; n escapes, must be boxed
```
**Solution:** Closure analysis already sets `needs_box = true`

### 3. Conditional Types
```clojure
(let [n (if foo 5 bar)]  ; bar might not be int
  (cpp/+ n 1))
```
**Solution:** Only infer primitive type if ALL branches return same primitive type

### 4. Return Type from C++ Calls
```clojure
(let [dd (imgui/GetDrawData)]  ; Returns ImDrawData*
  (cpp/.-CmdListsCount dd))    ; Field access on pointer
```
**Solution:** Use CppInterOp to get return type of function call

## Files to Modify

| File | Changes |
|------|---------|
| `src/cpp/jank/analyze/cpp_util.cpp` | Add primitive type detection in `expression_type()` |
| `src/cpp/jank/codegen/processor.cpp` | Re-enable `handle::str()` unboxed path |
| `src/cpp/jank/codegen/processor.cpp` | Update `gen(let_ref)` for unboxed locals |
| `src/cpp/jank/codegen/processor.cpp` | Update `gen(local_reference_ref)` |
| (Optional) `src/cpp/jank/analyze/pass/unbox_inference.cpp` | New pass for usage analysis |

## Testing Strategy

### Unit Tests
1. `expression_type(primitive_literal(5))` returns `long` type
2. `local_binding.type` is `long` for `[n 5]`
3. Generated code uses `jank::i64 n(5);` not `object_ref`

### Integration Tests
```clojure
;; Basic arithmetic
(let [n 5 m 10]
  (assert (= 15 (cpp/+ n m)))
  (assert (= 5 (cpp/- m n))))

;; C++ interop with field access
(let [dd (imgui/GetDrawData)]
  (dotimes [n (cpp/.-CmdListsCount dd)]
    (cpp/aget (cpp/.-CmdLists dd) n)))

;; Mixed usage (both boxed and unboxed)
(let [n 5]
  (println n)
  (cpp/+ n 10))
```

## Alternative Approaches Considered

### A. Explicit Type Hints
```clojure
(let [^long n 5]  ; User specifies type
  ...)
```
**Pros:** Explicit control
**Cons:** Verbose, not ergonomic

### B. Lazy Unboxing at Use Site
```clojure
(let [n 5]        ; n is boxed
  (cpp/+ n m))    ; Automatically unbox n and m here
```
**Pros:** No local changes
**Cons:** Runtime cost of unboxing, doesn't work for all contexts

### C. Hybrid (Recommended)
Combine automatic inference with optional explicit hints:
- Infer types automatically from literals and C++ returns
- Allow `^type` hints for overrides
- Support metadata like `^:unboxed` for explicit control

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Breaking existing code | Feature flag, gradual rollout |
| Performance regression | Benchmark before/after |
| Complex type inference bugs | Comprehensive test suite |
| Closure capture issues | Reuse existing `needs_box` logic |

## Success Criteria

1. Example code works without manual type annotations
2. No performance regression for existing code
3. Clear error messages when types can't be inferred
4. Integration with existing `cpp/` macros and operators
