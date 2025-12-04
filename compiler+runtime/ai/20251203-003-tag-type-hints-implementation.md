# Automatic Type Inference for Vars

Date: 2025-12-03

## Summary

Implemented automatic type inference for vars containing boxed C++ pointers. The key mechanism:
1. Store `boxed_type` directly on the var object (persists across processor instances)
2. `cpp/unbox` single-arg form reads `tag_type` from `var_deref` to know what to cast to

## Usage

```clojure
;; Create a pointer var - type is inferred from the literal
(defonce is-paused (v->p false))  ; creates bool*

;; Pass pointer to C++ functions (e.g., ImGui)
(imgui/Checkbox "Paused" (&p is-paused))  ; passes bool*

;; Read the value (dereference)
(if (p->v is-paused) ...)  ; returns bool
```

The key improvement: **no manual type specification** - `v->p` infers the type from the literal,
`&p` unboxes to the raw pointer, and `p->v` dereferences to get the value.

## Macros (vybe/util.jank)

### v->p macro - create a boxed pointer
```clojure
(defmacro v->p
  [v & [type-str]]
  (let [cpp-type (or type-str (infer-cpp-type v))]
    `(cpp/box (cpp/new (cpp/type ~cpp-type) ~v))))
```

### &p macro - get raw pointer (for C++ functions)
```clojure
(defmacro &p
  [p]
  `(cpp/unbox ~p))
```

### p->v macro - dereference a pointer
```clojure
(defmacro p->v
  [p]
  `(cpp/* (cpp/unbox ~p)))
```

## Compiler Implementation (How It Works)

### The Problem: Processor Instances Don't Share State

Each top-level form is analyzed by a NEW processor instance. The `vars` map in the processor
does not persist across these instances. So when `defonce is-paused` is analyzed in one
processor, and `(cpp/unbox is-paused)` is analyzed in another, the vars map is empty.

### The Solution: Store Type on Var Object

The var object itself is shared globally. We added a `boxed_type` field directly to the
`struct var` in `var.hpp`:

```cpp
struct var : gc {
  // ...existing fields...

  /* The C++ type of the var's boxed value (for cpp/box inference).
   * Set during analysis when the init expression is a cpp_box.
   * nullptr means no type hint available. */
  jtl::ptr<void> boxed_type{};
};
```

### Data Flow

1. **analyze_def**: When analyzing `(def x (cpp/box ...))`:
   - Store original expression in vars map (for same-processor lookups)
   - If expression is `cpp_box`, store its `boxed_type` on `var->boxed_type`

2. **analyze_symbol**: When a var is referenced:
   - First check `var->boxed_type` (persists across processors)
   - Fall back to vars map (same-processor only)
   - Fall back to `:tag` metadata (for pre-compiled modules)
   - Store result in `var_deref.tag_type`

3. **expression_type(var_deref)**: Always returns `object*`
   - Vars hold `object*` at runtime (the boxed value)
   - The `tag_type` is just a hint for unboxing, not the expression type

4. **analyze_cpp_unbox (single-arg form)**: `(cpp/unbox var)`
   - Get `tag_type` directly from `var_deref` expression
   - Use `expression_type` to verify value is `object*` (can be unboxed)
   - Use `tag_type` as the unbox target type

### Key Insight

- `cpp_box` returns `object*` at runtime
- `var->boxed_type` stores the underlying type (e.g., `bool*`)
- `expression_type(var_deref)` = `object*` (runtime type)
- `var_deref.tag_type` = `bool*` (unbox target type)

## Fallback: Manual `:tag` Metadata

For pre-compiled modules or explicit overrides, `:tag` metadata still works:

```clojure
(def ^:bool manual-var ...)
```

Supported `:tag` keywords:
- `:bool` / `:boolean` -> `bool*`
- `:i8`, `:i16`, `:i32`/`:int`, `:i64`/`:long`
- `:u8`, `:u16`, `:u32`, `:u64`
- `:f32`/`:float`, `:f64`/`:double`
- `:size_t`, `:char`

## Files Modified

1. **var.hpp** (line 79)
   - Added `boxed_type` field to var struct

2. **processor.cpp** (analyze_def, ~line 1356)
   - Store original expression in vars map before implicit conversion
   - If cpp_box, store boxed_type on var object

3. **processor.cpp** (analyze_symbol, ~line 1779)
   - Check var->boxed_type first (persists across processors)
   - Fall back to vars map, then `:tag` metadata

4. **processor.cpp** (analyze_cpp_unbox, ~line 4728)
   - Single-arg form gets tag_type directly from var_deref
   - Use expression_type only to verify value is object*

5. **cpp_util.cpp** (expression_type, var_deref case)
   - Always return object* (vars hold object* at runtime)
   - tag_type is accessed directly, not via expression_type

## Implementation Details

### Type Checking Pattern

Jank uses its own RTTI system:

```cpp
if(tag->type == runtime::object_type::keyword)
{
  auto const kw = runtime::expect_object<runtime::obj::keyword>(tag);
  auto const &name = kw->get_name();
  // ...
}
```

### Includes Required

```cpp
#include <jank/runtime/obj/keyword.hpp>
#include <jank/runtime/obj/persistent_string.hpp>
#include <jank/runtime/rtti.hpp>
```
